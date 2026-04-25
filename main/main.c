/*
 * Elering Smart Relay — ESP32-C3
 *
 * Boot: read NVS creds → try STA → fallback AP "ElereRelay-Setup"
 * Pages: /          price table
 *        /wifi      change WiFi credentials
 *        /settings  runtime settings (window, cheap hours, relay invert,
 *                   fetch schedule, max display rows, NTP, timezone, MQTT)
 *
 * All settings survive reboots (NVS). Changes on /settings apply instantly.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_system.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_sntp.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "mqtt_client.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "display.h"

static const char *TAG = "elerelay";

/* ── Config ──────────────────────────────────────────────────────────────── */

#define RELAY_GPIO       ((gpio_num_t)CONFIG_RELAY_GPIO)
#define RELAY_ON_LEVEL   (CONFIG_RELAY_ACTIVE_LOW ? 0 : 1)
#define RELAY_OFF_LEVEL  (CONFIG_RELAY_ACTIVE_LOW ? 1 : 0)

/* On-board RGB LED — active LOW */
#define LED_RED_GPIO    4
#define LED_GREEN_GPIO  16
#define LED_BLUE_GPIO   17
#define LED_ON  0
#define LED_OFF 1

#define MAX_SLOTS        112         /* 24 h × 4 slots/h + spare             */
#define HTTP_BUF_SIZE    (24 * 1024)
#define WIFI_MAX_RETRY   10

#define AP_SSID          "ElereRelay-Setup"
#define NVS_NS           "elerelay"
/* WiFi credential keys */
#define NVS_KEY_SSID     "wifi_ssid"
#define NVS_KEY_PASS     "wifi_pass"
/* Settings keys */
#define NVS_KEY_WINDOW   "h_window"
#define NVS_KEY_CHEAP    "cheap_h_n"
#define NVS_KEY_INV      "relay_inv"
#define NVS_KEY_FETCHH   "fetch_hour"
#define NVS_KEY_MAXDISP  "max_disp"
#define NVS_KEY_NTP_SRV  "ntp_server"
#define NVS_KEY_TZ       "tz_str"
#define NVS_KEY_MQTT_EN  "mqtt_en"
#define NVS_KEY_MQTT_HOST "mqtt_host"
#define NVS_KEY_MQTT_PORT "mqtt_port"
#define NVS_KEY_MQTT_TPRC "mqtt_topic_p"
#define NVS_KEY_MQTT_TRLY "mqtt_topic_r"

#define CRED_LEN         64
#define STR_LEN          64

/* ── Runtime settings (loaded from NVS, defaults from Kconfig) ───────────── */

static int  s_hours_window = CONFIG_HOURS_WINDOW;  /* 2-24               */
static int  s_cheap_hours  = CONFIG_CHEAP_HOURS;   /* 1 .. window-1      */
static bool s_relay_inv    = false;                /* invert relay logic */
static int  s_fetch_hour   = 23;                   /* 0-23               */
static int  s_max_display  = 48;                   /* max rows in table  */

static char s_ntp_server[STR_LEN]       = "pool.ntp.org";
static char s_tz_str[STR_LEN]           = "EET-2EEST,M3.5.0/3,M10.5.0/4";
static bool s_mqtt_enabled              = false;
static char s_mqtt_host[STR_LEN]        = "";
static int  s_mqtt_port                 = 1883;
static char s_mqtt_topic_price[STR_LEN] = "elerelay/price";
static char s_mqtt_topic_relay[STR_LEN] = "elerelay/relay";

/* ── Types ───────────────────────────────────────────────────────────────── */

typedef struct {
    time_t ts;
    float  price;
    bool   cheap;
} hour_slot_t;

/* ── Globals ─────────────────────────────────────────────────────────────── */

static EventGroupHandle_t s_wifi_eg;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static int s_retry_num = 0;

static SemaphoreHandle_t s_mutex;
static hour_slot_t       s_hours[MAX_SLOTS];
static int               s_hour_count = 0;
static bool              s_relay_on   = false;
static time_t            s_last_fetch = 0;
static char              s_ip[20]     = "?.?.?.?";
static bool              s_ap_mode    = false;
static char              s_wifi_ssid[CRED_LEN] = {0};

static char *s_http_buf = NULL;
static int   s_http_len = 0;

static esp_mqtt_client_handle_t s_mqtt_client = NULL;

/* ── NVS: WiFi credentials ───────────────────────────────────────────────── */

static esp_err_t nvs_load_creds(char *ssid, char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t len = CRED_LEN;
    err = nvs_get_str(h, NVS_KEY_SSID, ssid, &len);
    if (err == ESP_OK) { len = CRED_LEN; err = nvs_get_str(h, NVS_KEY_PASS, pass, &len); }
    nvs_close(h);
    return err;
}

static esp_err_t nvs_save_creds(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, NVS_KEY_SSID, ssid);
    if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_PASS, pass);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* ── NVS: runtime settings ───────────────────────────────────────────────── */

static void nvs_load_settings(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    uint8_t v;
    if (nvs_get_u8(h, NVS_KEY_WINDOW,  &v) == ESP_OK) s_hours_window = v;
    if (nvs_get_u8(h, NVS_KEY_CHEAP,   &v) == ESP_OK) s_cheap_hours  = v;
    if (nvs_get_u8(h, NVS_KEY_INV,     &v) == ESP_OK) s_relay_inv    = (bool)v;
    if (nvs_get_u8(h, NVS_KEY_FETCHH,  &v) == ESP_OK) s_fetch_hour   = v;
    if (nvs_get_u8(h, NVS_KEY_MAXDISP, &v) == ESP_OK) s_max_display  = v;
    if (nvs_get_u8(h, NVS_KEY_MQTT_EN, &v) == ESP_OK) s_mqtt_enabled = (bool)v;

    uint16_t port;
    if (nvs_get_u16(h, NVS_KEY_MQTT_PORT, &port) == ESP_OK) s_mqtt_port = port;

    size_t len;
    len = STR_LEN; nvs_get_str(h, NVS_KEY_NTP_SRV,  s_ntp_server,       &len);
    len = STR_LEN; nvs_get_str(h, NVS_KEY_TZ,        s_tz_str,           &len);
    len = STR_LEN; nvs_get_str(h, NVS_KEY_MQTT_HOST, s_mqtt_host,        &len);
    len = STR_LEN; nvs_get_str(h, NVS_KEY_MQTT_TPRC, s_mqtt_topic_price, &len);
    len = STR_LEN; nvs_get_str(h, NVS_KEY_MQTT_TRLY, s_mqtt_topic_relay, &len);

    nvs_close(h);
}

static esp_err_t nvs_save_settings(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err  = nvs_set_u8(h,  NVS_KEY_WINDOW,    (uint8_t)s_hours_window);
    err |= nvs_set_u8(h,  NVS_KEY_CHEAP,     (uint8_t)s_cheap_hours);
    err |= nvs_set_u8(h,  NVS_KEY_INV,       (uint8_t)s_relay_inv);
    err |= nvs_set_u8(h,  NVS_KEY_FETCHH,    (uint8_t)s_fetch_hour);
    err |= nvs_set_u8(h,  NVS_KEY_MAXDISP,   (uint8_t)s_max_display);
    err |= nvs_set_u8(h,  NVS_KEY_MQTT_EN,   (uint8_t)s_mqtt_enabled);
    err |= nvs_set_u16(h, NVS_KEY_MQTT_PORT,  (uint16_t)s_mqtt_port);
    err |= nvs_set_str(h, NVS_KEY_NTP_SRV,   s_ntp_server);
    err |= nvs_set_str(h, NVS_KEY_TZ,        s_tz_str);
    err |= nvs_set_str(h, NVS_KEY_MQTT_HOST, s_mqtt_host);
    err |= nvs_set_str(h, NVS_KEY_MQTT_TPRC, s_mqtt_topic_price);
    err |= nvs_set_str(h, NVS_KEY_MQTT_TRLY, s_mqtt_topic_relay);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* ── URL / form helpers ──────────────────────────────────────────────────── */

static void url_decode(char *dst, const char *src, size_t maxlen)
{
    size_t i = 0;
    while (*src && i + 1 < maxlen) {
        if (*src == '+') {
            dst[i++] = ' '; src++;
        } else if (*src == '%'
                   && isxdigit((unsigned char)src[1])
                   && isxdigit((unsigned char)src[2])) {
            int hi = isdigit((unsigned char)src[1]) ? src[1]-'0' : tolower((unsigned char)src[1])-'a'+10;
            int lo = isdigit((unsigned char)src[2]) ? src[2]-'0' : tolower((unsigned char)src[2])-'a'+10;
            dst[i++] = (char)(hi * 16 + lo);
            src += 3;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

static void form_field(const char *body, const char *key,
                       char *out, size_t outlen)
{
    char needle[72];
    snprintf(needle, sizeof(needle), "%s=", key);
    const char *p = strstr(body, needle);
    if (!p) { out[0] = '\0'; return; }
    p += strlen(needle);
    const char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    char raw[CRED_LEN * 3] = {0};
    if (len >= sizeof(raw)) len = sizeof(raw) - 1;
    memcpy(raw, p, len);
    url_decode(out, raw, outlen);
}

static void html_escape(const char *in, char *out, size_t outlen)
{
    size_t i = 0;
    while (*in && i + 7 < outlen) {
        if      (*in == '<') { memcpy(out+i, "&lt;",   4); i += 4; }
        else if (*in == '>') { memcpy(out+i, "&gt;",   4); i += 4; }
        else if (*in == '&') { memcpy(out+i, "&amp;",  5); i += 5; }
        else if (*in == '"') { memcpy(out+i, "&quot;", 6); i += 6; }
        else                 { out[i++] = *in; }
        in++;
    }
    out[i] = '\0';
}

/* clamp an int to [lo, hi] */
static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ── WiFi ────────────────────────────────────────────────────────────────── */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect(); s_retry_num++;
            ESP_LOGW(TAG, "WiFi retry %d/%d", s_retry_num, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "WiFi failed after %d retries", WIFI_MAX_RETRY);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&evt->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "WiFi connected — IP: %s", s_ip);
    }
}

static bool wifi_start_sta(const char *ssid, const char *pass)
{
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();

    /* Set hostname to EleRelay-XXYYZZ (last 3 bytes of MAC) */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char hostname[24];
    snprintf(hostname, sizeof(hostname), "EleRelay-%02X%02X%02X",
             mac[3], mac[4], mac[5]);
    esp_netif_set_hostname(sta, hostname);
    ESP_LOGI(TAG, "Hostname: %s", hostname);

    wifi_config_t wcfg = {0};
    strlcpy((char *)wcfg.sta.ssid,     ssid, sizeof(wcfg.sta.ssid));
    strlcpy((char *)wcfg.sta.password, pass, sizeof(wcfg.sta.password));
    if (strlen(pass) > 0) wcfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_eg, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, portMAX_DELAY);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

static void wifi_start_ap(void)
{
    esp_wifi_stop();
    esp_netif_create_default_wifi_ap();
    wifi_config_t ap_cfg = {0};
    strlcpy((char *)ap_cfg.ap.ssid, AP_SSID, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(AP_SSID);
    ap_cfg.ap.channel  = 6;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ap_cfg.ap.max_connection = 4;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    snprintf(s_ip, sizeof(s_ip), "192.168.4.1");
    ESP_LOGI(TAG, "AP started — SSID: %s  IP: 192.168.4.1", AP_SSID);
}

/* ── NTP ─────────────────────────────────────────────────────────────────── */

static void ntp_sync(void)
{
    setenv("TZ", s_tz_str, 1);
    tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, s_ntp_server);
    esp_sntp_setservername(1, "time.cloudflare.com");
    esp_sntp_init();
    time_t now = 0; struct tm ti = {0};
    for (int i = 0; ti.tm_year < (2020-1900) && i < 40; i++) {
        ESP_LOGI(TAG, "NTP sync... (%d)", i+1);
        vTaskDelay(pdMS_TO_TICKS(2000));
        time(&now); localtime_r(&now, &ti);
    }
    char buf[32]; strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &ti);
    ESP_LOGI(TAG, "Time: %s", buf);
}

/* ── MQTT ────────────────────────────────────────────────────────────────── */

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    if      (id == MQTT_EVENT_CONNECTED)    ESP_LOGI(TAG, "MQTT connected");
    else if (id == MQTT_EVENT_DISCONNECTED) ESP_LOGW(TAG, "MQTT disconnected");
    else if (id == MQTT_EVENT_ERROR)        ESP_LOGE(TAG, "MQTT error");
}

static void mqtt_start(void)
{
    /* Tear down any existing client */
    if (s_mqtt_client) {
        esp_mqtt_client_stop(s_mqtt_client);
        esp_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
    }
    if (!s_mqtt_enabled || s_mqtt_host[0] == '\0') return;

    char uri[96];
    snprintf(uri, sizeof(uri), "mqtt://%s:%d", s_mqtt_host, s_mqtt_port);
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
    };
    s_mqtt_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt_client);
    ESP_LOGI(TAG, "MQTT → %s", uri);
}

static void mqtt_publish(const char *topic, const char *payload)
{
    if (!s_mqtt_client || !s_mqtt_enabled || !topic || topic[0] == '\0') return;
    esp_mqtt_client_publish(s_mqtt_client, topic, payload, 0, /*qos*/0, /*retain*/1);
}

/* ── Elering API ─────────────────────────────────────────────────────────── */

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (s_http_buf && s_http_len + evt->data_len < HTTP_BUF_SIZE - 1) {
            memcpy(s_http_buf + s_http_len, evt->data, evt->data_len);
            s_http_len += evt->data_len;
        }
    } else if (evt->event_id == HTTP_EVENT_ON_FINISH) {
        if (s_http_buf) s_http_buf[s_http_len] = '\0';
    }
    return ESP_OK;
}

/* ── Price logic ─────────────────────────────────────────────────────────── */

static int cmp_price_asc(const void *a, const void *b)
{
    float fa = ((const hour_slot_t *)a)->price;
    float fb = ((const hour_slot_t *)b)->price;
    return (fa > fb) - (fa < fb);
}

/* Mark the cheapest 15-min slots as cheap=true.
 * s_cheap_hours is in hours; multiply by 4 to get 15-min slot count. */
static void mark_cheap_hours(hour_slot_t *slots, int count)
{
    int cheap_slots = s_cheap_hours * 4;
    int n = count < cheap_slots ? count : cheap_slots;
    if (n <= 0) { for (int i = 0; i < count; i++) slots[i].cheap = false; return; }
    hour_slot_t tmp[MAX_SLOTS];
    memcpy(tmp, slots, count * sizeof(hour_slot_t));
    qsort(tmp, count, sizeof(hour_slot_t), cmp_price_asc);
    float threshold = tmp[n-1].price;
    int marked = 0;
    for (int i = 0; i < count; i++) {
        if (marked < n && slots[i].price <= threshold) { slots[i].cheap = true; marked++; }
        else slots[i].cheap = false;
    }
}

static esp_err_t fetch_prices(void)
{
    time_t now; time(&now);
    time_t win_start = (now / 3600) * 3600;
    time_t win_end   = win_start + (s_hours_window + 2) * 3600;

    struct tm ts, te;
    gmtime_r(&win_start, &ts);
    gmtime_r(&win_end,   &te);

    char url[300];
    snprintf(url, sizeof(url),
        "https://dashboard.elering.ee/api/nps/price"
        "?fields=ee"
        "&start=%04d-%02d-%02dT%02d:00:00.000Z"
        "&end=%04d-%02d-%02dT%02d:00:00.000Z",
        ts.tm_year+1900, ts.tm_mon+1, ts.tm_mday, ts.tm_hour,
        te.tm_year+1900, te.tm_mon+1, te.tm_mday, te.tm_hour);
    ESP_LOGI(TAG, "GET %s", url);

    s_http_buf = calloc(1, HTTP_BUF_SIZE);
    if (!s_http_buf) return ESP_ERR_NO_MEM;
    s_http_len = 0;

    esp_http_client_config_t cfg = {
        .url               = url,
        .event_handler     = http_event_cb,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 20000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t err    = esp_http_client_perform(client);
    int       status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "HTTP %s / %d", esp_err_to_name(err), status);
        free(s_http_buf); s_http_buf = NULL;
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(s_http_buf);
    free(s_http_buf); s_http_buf = NULL;
    if (!root) { ESP_LOGE(TAG, "JSON parse failed"); return ESP_FAIL; }

    cJSON *ee = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "data"), "ee");
    if (!cJSON_IsArray(ee)) {
        ESP_LOGE(TAG, "No data.ee array"); cJSON_Delete(root); return ESP_FAIL;
    }

    /* Store each 15-min slot directly — no hourly aggregation */
    hour_slot_t raw[MAX_SLOTS];
    int nraw = 0;

    cJSON *item;
    cJSON_ArrayForEach(item, ee) {
        cJSON *ts_j = cJSON_GetObjectItem(item, "timestamp");
        cJSON *pr_j = cJSON_GetObjectItem(item, "price");
        if (!cJSON_IsNumber(ts_j) || !cJSON_IsNumber(pr_j)) continue;
        time_t slot_ts = (time_t)ts_j->valuedouble;
        if (slot_ts < win_start || nraw >= MAX_SLOTS) continue;
        raw[nraw].ts    = slot_ts;
        raw[nraw].price = (float)pr_j->valuedouble;
        raw[nraw].cheap = false;
        nraw++;
    }
    cJSON_Delete(root);

    /* Sort chronologically (API order may vary) */
    for (int i = 0; i < nraw-1; i++)
        for (int j = i+1; j < nraw; j++)
            if (raw[j].ts < raw[i].ts) {
                hour_slot_t t = raw[i]; raw[i] = raw[j]; raw[j] = t;
            }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_hour_count = nraw;
    memcpy(s_hours, raw, nraw * sizeof(hour_slot_t));
    mark_cheap_hours(s_hours, s_hour_count);
    s_last_fetch = now;
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Parsed %d × 15-min slots", s_hour_count);
    return ESP_OK;
}

static void update_display(void);   /* forward declaration */

/* ── Relay ───────────────────────────────────────────────────────────────── */

static void update_relay(void)
{
    time_t now; time(&now);
    time_t cur_slot = (now / 900) * 900;    /* round down to 15-min boundary */
    bool  is_cheap   = false;
    bool  found_slot = false;
    float cur_price  = 0.0f;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_hour_count; i++) {
        if (s_hours[i].ts == cur_slot) {
            is_cheap   = s_hours[i].cheap;
            cur_price  = s_hours[i].price / 10.0f;   /* EUR/MWh → c/kWh */
            found_slot = true;
            break;
        }
    }
    bool on = is_cheap ^ s_relay_inv;   /* invert flips cheap↔expensive */
    s_relay_on = on;
    xSemaphoreGive(s_mutex);

    gpio_set_level(RELAY_GPIO, on ? RELAY_ON_LEVEL : RELAY_OFF_LEVEL);

    /* RGB LED: green = cheap, red = expensive, blue always off */
    gpio_set_level(LED_GREEN_GPIO, is_cheap ? LED_ON : LED_OFF);
    gpio_set_level(LED_RED_GPIO,   is_cheap ? LED_OFF : LED_ON);
    gpio_set_level(LED_BLUE_GPIO,  LED_OFF);

    ESP_LOGI(TAG, "Relay → %s (hour %s, inv=%d)",
             on ? "ON" : "OFF", is_cheap ? "cheap" : "expensive", s_relay_inv);

    /* MQTT publish relay state and current price */
    mqtt_publish(s_mqtt_topic_relay, on ? "ON" : "OFF");
    if (found_slot) {
        char pbuf[16];
        snprintf(pbuf, sizeof(pbuf), "%.3f", cur_price);
        mqtt_publish(s_mqtt_topic_price, pbuf);
    }
}

/* ── Restart helper ──────────────────────────────────────────────────────── */

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
}

/* ── Common CSS ──────────────────────────────────────────────────────────── */

static const char *PAGE_CSS =
    "<style>"
    "body{font-family:system-ui,sans-serif;max-width:680px;margin:30px auto;"
         "padding:0 16px;color:#222}"
    "h1{font-size:1.4rem;margin-bottom:4px}"
    "h2{font-size:1.1rem;margin:22px 0 4px;padding-top:16px;"
        "border-top:1px solid #ddd;color:#444}"
    "nav{margin-bottom:14px;font-size:.88rem}"
    "nav a{color:#1565c0;margin-right:12px}"
    ".status{display:inline-block;padding:3px 11px;border-radius:20px;"
            "font-weight:600}"
    ".on{background:#c8e6c9;color:#1b5e20}"
    ".off{background:#ffcdd2;color:#b71c1c}"
    "table{width:100%;border-collapse:collapse;margin-top:16px;font-size:.9rem}"
    "th,td{padding:7px 10px;border:1px solid #ddd;text-align:center}"
    "th{background:#f5f5f5;font-weight:600}"
    "tr.cheap td{background:#e8f5e9}"
    "tr.exp   td{background:#ffebee}"
    "tr.cur   td{outline:2px solid #333;outline-offset:-1px;font-weight:700}"
    "tr.hg td:first-child{border-left:2px solid #888}"
    "tr.hg td:last-child{border-right:2px solid #888}"
    "tr.hs td{border-top:2px solid #888}"
    "tr.he td{border-bottom:2px solid #888}"
    "td.ron{color:#2e7d32;font-weight:600}"
    "td.roff{color:#c62828;font-weight:600}"
    ".meta{color:#666;font-size:.82rem;margin:3px 0}"
    "label{display:block;margin-top:14px;font-size:.9rem;color:#555}"
    "select,input[type=number],input[type=text],input[type=password]{"
        "width:100%;box-sizing:border-box;padding:7px;margin-top:4px;"
        "border:1px solid #ccc;border-radius:6px;font-size:1rem}"
    "select{background:#fff}"
    ".chk{display:flex;align-items:center;gap:8px;margin-top:14px;"
          "font-size:.9rem;color:#555}"
    ".chk input{width:auto;margin:0}"
    "button{margin-top:20px;width:100%;padding:10px;background:#1565c0;"
            "color:#fff;border:none;border-radius:6px;font-size:1rem;cursor:pointer}"
    "button:hover{background:#0d47a1}"
    ".note{color:#666;font-size:.82rem;margin-top:8px}"
    "progress{width:100%;height:22px;margin-top:12px;border-radius:4px}"
    ".ota-status{margin-top:10px;font-size:.95rem;font-weight:600}"
    "</style>";

static void send_page_head(httpd_req_t *req, const char *title)
{
    char buf[256];
    httpd_resp_set_type(req, "text/html; charset=UTF-8");
    httpd_resp_sendstr_chunk(req, "<!DOCTYPE html><html lang='en'><head>"
        "<meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>");
    snprintf(buf, sizeof(buf), "<title>%s</title>", title);
    httpd_resp_sendstr_chunk(req, buf);
    httpd_resp_sendstr_chunk(req, PAGE_CSS);
    httpd_resp_sendstr_chunk(req, "</head><body>"
        "<nav>"
        "<a href='/'>&#x26A1; Prices</a>"
        "<a href='/settings'>&#x2699;&#xFE0F; Settings</a>"
        "<a href='/wifi'>&#x1F4F6; WiFi</a>"
        "<a href='/ota'>&#x1F4E6; OTA</a>"
        "</nav>");
}

/* ── /settings GET ───────────────────────────────────────────────────────── */

static esp_err_t settings_get_handler(httpd_req_t *req)
{
    send_page_head(req, "ElereRelay \xe2\x80\x94 Settings");
    httpd_resp_sendstr_chunk(req, "<h1>&#x2699;&#xFE0F; Settings</h1>"
        "<form method='POST' action='/settings'>");

    /* ── Relay / price settings ── */
    httpd_resp_sendstr_chunk(req, "<h2>Relay &amp; Price</h2>");

    /* Hours window dropdown */
    char chunk[256];
    httpd_resp_sendstr_chunk(req, "<label>Hours window"
        "<select name='window'>");
    for (int i = 2; i <= 24; i++) {
        snprintf(chunk, sizeof(chunk), "<option value='%d'%s>%d hours</option>",
                 i, i == s_hours_window ? " selected" : "", i);
        httpd_resp_sendstr_chunk(req, chunk);
    }
    httpd_resp_sendstr_chunk(req, "</select></label>");

    /* Cheap hours number */
    {
        char buf[192];
        snprintf(buf, sizeof(buf),
            "<label>Cheap hours (N&times;4 cheapest 15-min slots = ON)"
            "<input type='number' name='cheap' min='1' max='23' value='%d'>"
            "</label>", s_cheap_hours);
        httpd_resp_sendstr_chunk(req, buf);
    }

    /* Relay inverted checkbox */
    {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "<div class='chk'>"
            "<input type='checkbox' name='inv' id='inv'%s>"
            "<label for='inv' style='margin:0;color:#222'>"
            "Inverted &mdash; ON during expensive hours</label>"
            "</div>", s_relay_inv ? " checked" : "");
        httpd_resp_sendstr_chunk(req, buf);
    }

    /* Fetch hour dropdown */
    httpd_resp_sendstr_chunk(req, "<label>Fetch prices at (daily)"
        "<select name='fetch_h'>");
    for (int h = 0; h < 24; h++) {
        snprintf(chunk, sizeof(chunk), "<option value='%d'%s>%02d:00</option>",
                 h, h == s_fetch_hour ? " selected" : "", h);
        httpd_resp_sendstr_chunk(req, chunk);
    }
    httpd_resp_sendstr_chunk(req, "</select></label>"
        "<p class='note'>Elering publishes next-day prices around 14:00 EET."
        " Fetching at 23:00 ensures a full set of tomorrow&rsquo;s prices.</p>");

    /* Max display rows */
    {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "<label>Max slots on price page (4 = 1 h)"
            "<input type='number' name='max_disp' min='1' max='%d' value='%d'>"
            "</label>", MAX_SLOTS, s_max_display);
        httpd_resp_sendstr_chunk(req, buf);
    }

    /* ── Time settings ── */
    httpd_resp_sendstr_chunk(req, "<h2>Time</h2>");

    /* NTP server */
    {
        char safe[STR_LEN * 6] = {0};
        html_escape(s_ntp_server, safe, sizeof(safe));
        char buf[512];
        snprintf(buf, sizeof(buf),
            "<label>NTP server"
            "<input type='text' name='ntp_server' maxlength='%d' value='%s'>"
            "</label>", STR_LEN - 1, safe);
        httpd_resp_sendstr_chunk(req, buf);
    }

    /* Timezone POSIX TZ string */
    {
        char safe[STR_LEN * 6] = {0};
        html_escape(s_tz_str, safe, sizeof(safe));
        char buf[768];
        snprintf(buf, sizeof(buf),
            "<label>Timezone (POSIX TZ string)"
            "<input type='text' name='tz_str' maxlength='%d' value='%s'>"
            "</label>"
            "<p class='note'>Examples: "
            "<code>EET-2EEST,M3.5.0/3,M10.5.0/4</code> (Estonia), "
            "<code>UTC0</code>, "
            "<code>CET-1CEST,M3.5.0,M10.5.0/3</code> (Central Europe)</p>",
            STR_LEN - 1, safe);
        httpd_resp_sendstr_chunk(req, buf);
    }

    /* ── MQTT settings ── */
    httpd_resp_sendstr_chunk(req, "<h2>MQTT</h2>");

    /* MQTT enabled checkbox */
    {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "<div class='chk'>"
            "<input type='checkbox' name='mqtt_en' id='mqtt_en'%s>"
            "<label for='mqtt_en' style='margin:0;color:#222'>"
            "Enable MQTT publishing</label>"
            "</div>", s_mqtt_enabled ? " checked" : "");
        httpd_resp_sendstr_chunk(req, buf);
    }

    /* MQTT server */
    {
        char safe[STR_LEN * 6] = {0};
        html_escape(s_mqtt_host, safe, sizeof(safe));
        char buf[640];
        snprintf(buf, sizeof(buf),
            "<label>MQTT server hostname / IP"
            "<input type='text' name='mqtt_host' maxlength='%d' value='%s'"
            " placeholder='e.g. 192.168.1.10 or broker.local'>"
            "</label>", STR_LEN - 1, safe);
        httpd_resp_sendstr_chunk(req, buf);
    }

    /* MQTT port */
    {
        char buf[192];
        snprintf(buf, sizeof(buf),
            "<label>MQTT port"
            "<input type='number' name='mqtt_port' min='1' max='65535' value='%d'>"
            "</label>", s_mqtt_port);
        httpd_resp_sendstr_chunk(req, buf);
    }

    /* MQTT price topic */
    {
        char safe[STR_LEN * 6] = {0};
        html_escape(s_mqtt_topic_price, safe, sizeof(safe));
        char buf[512];
        snprintf(buf, sizeof(buf),
            "<label>Price topic"
            "<input type='text' name='mqtt_topic_p' maxlength='%d' value='%s'>"
            "</label>", STR_LEN - 1, safe);
        httpd_resp_sendstr_chunk(req, buf);
    }

    /* MQTT relay topic */
    {
        char safe[STR_LEN * 6] = {0};
        html_escape(s_mqtt_topic_relay, safe, sizeof(safe));
        char buf[640];
        snprintf(buf, sizeof(buf),
            "<label>Relay state topic"
            "<input type='text' name='mqtt_topic_r' maxlength='%d' value='%s'>"
            "</label>"
            "<p class='note'>Publishes price in c/kWh and relay state"
            " (<code>ON</code>/<code>OFF</code>) with retain=1 on every"
            " relay update.</p>", STR_LEN - 1, safe);
        httpd_resp_sendstr_chunk(req, buf);
    }

    httpd_resp_sendstr_chunk(req,
        "<button type='submit'>Save settings</button>"
        "</form></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* ── /settings POST ──────────────────────────────────────────────────────── */

static esp_err_t settings_post_handler(httpd_req_t *req)
{
    char body[1024] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty"); return ESP_FAIL; }
    body[len] = '\0';

    char val[STR_LEN];

    form_field(body, "window",   val, sizeof(val));
    int window = clamp(atoi(val), 2, 24);

    form_field(body, "cheap",    val, sizeof(val));
    int cheap = clamp(atoi(val), 1, window - 1 < 1 ? 1 : window - 1);

    /* checkbox: present = checked, absent = unchecked */
    bool inv = (strstr(body, "inv=on") || strstr(body, "inv=1"));

    form_field(body, "fetch_h",  val, sizeof(val));
    int fetch_h = clamp(atoi(val), 0, 23);

    form_field(body, "max_disp", val, sizeof(val));
    int max_disp = clamp(atoi(val), 1, MAX_SLOTS);

    /* Time settings */
    char ntp_server[STR_LEN] = {0};
    form_field(body, "ntp_server", ntp_server, sizeof(ntp_server));
    if (ntp_server[0] == '\0') strlcpy(ntp_server, "pool.ntp.org", sizeof(ntp_server));

    char tz_str[STR_LEN] = {0};
    form_field(body, "tz_str", tz_str, sizeof(tz_str));
    if (tz_str[0] == '\0') strlcpy(tz_str, "UTC0", sizeof(tz_str));

    /* MQTT settings */
    bool mqtt_en = (strstr(body, "mqtt_en=on") || strstr(body, "mqtt_en=1"));

    char mqtt_host[STR_LEN] = {0};
    form_field(body, "mqtt_host", mqtt_host, sizeof(mqtt_host));

    form_field(body, "mqtt_port", val, sizeof(val));
    int mqtt_port = clamp(atoi(val), 1, 65535);
    if (mqtt_port == 0) mqtt_port = 1883;

    char mqtt_topic_p[STR_LEN] = {0};
    form_field(body, "mqtt_topic_p", mqtt_topic_p, sizeof(mqtt_topic_p));
    if (mqtt_topic_p[0] == '\0') strlcpy(mqtt_topic_p, "elerelay/price", sizeof(mqtt_topic_p));

    char mqtt_topic_r[STR_LEN] = {0};
    form_field(body, "mqtt_topic_r", mqtt_topic_r, sizeof(mqtt_topic_r));
    if (mqtt_topic_r[0] == '\0') strlcpy(mqtt_topic_r, "elerelay/relay", sizeof(mqtt_topic_r));

    /* Apply immediately */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_hours_window = window;
    s_cheap_hours  = cheap;
    s_relay_inv    = inv;
    s_fetch_hour   = fetch_h;
    s_max_display  = max_disp;
    strlcpy(s_ntp_server,       ntp_server,  sizeof(s_ntp_server));
    strlcpy(s_tz_str,           tz_str,      sizeof(s_tz_str));
    s_mqtt_enabled = mqtt_en;
    strlcpy(s_mqtt_host,        mqtt_host,   sizeof(s_mqtt_host));
    s_mqtt_port    = mqtt_port;
    strlcpy(s_mqtt_topic_price, mqtt_topic_p, sizeof(s_mqtt_topic_price));
    strlcpy(s_mqtt_topic_relay, mqtt_topic_r, sizeof(s_mqtt_topic_relay));
    mark_cheap_hours(s_hours, s_hour_count);   /* recompute with new cheap count */
    xSemaphoreGive(s_mutex);

    /* Apply timezone immediately */
    setenv("TZ", s_tz_str, 1);
    tzset();

    update_relay();          /* reflect new inversion + cheap hours instantly */
    nvs_save_settings();
    update_display();        /* reflect new settings on LCD immediately */
    mqtt_start();            /* restart MQTT client with new settings */

    ESP_LOGI(TAG, "Settings updated: window=%d cheap=%d inv=%d fetch_h=%d "
             "max_disp=%d ntp=%s tz=%s mqtt=%d host=%s port=%d tp=%s tr=%s",
             window, cheap, inv, fetch_h, max_disp,
             ntp_server, tz_str, mqtt_en, mqtt_host, mqtt_port,
             mqtt_topic_p, mqtt_topic_r);

    send_page_head(req, "ElereRelay \xe2\x80\x94 Settings");
    char chunk[768];
    snprintf(chunk, sizeof(chunk),
        "<h1>Settings</h1>"
        "<p>&#x2714; Saved &amp; applied.</p>"
        "<ul style='font-size:.9rem;line-height:1.8'>"
        "<li>Window: <b>%d h</b></li>"
        "<li>Cheap hours: <b>%d</b></li>"
        "<li>Relay inverted: <b>%s</b></li>"
        "<li>Price fetch at: <b>%02d:00</b> daily</li>"
        "<li>Max rows: <b>%d</b></li>"
        "<li>NTP server: <b>%s</b></li>"
        "<li>Timezone: <b>%s</b></li>"
        "<li>MQTT: <b>%s</b>%s</li>"
        "</ul>"
        "<p><a href='/settings'>Back to settings</a>"
        " &nbsp; <a href='/'>Price table</a></p>"
        "</body></html>",
        window, cheap, inv ? "yes" : "no", fetch_h, max_disp,
        ntp_server, tz_str,
        mqtt_en ? "enabled" : "disabled",
        mqtt_en && mqtt_host[0] ? " — connecting..." : "");
    httpd_resp_sendstr_chunk(req, chunk);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* ── /wifi GET ───────────────────────────────────────────────────────────── */

static void send_wifi_form(httpd_req_t *req, bool is_ap)
{
    send_page_head(req, "ElereRelay \xe2\x80\x94 WiFi");
    httpd_resp_sendstr_chunk(req, "<h1>&#x1F4F6; ");
    httpd_resp_sendstr_chunk(req, is_ap ? "First Setup" : "WiFi Settings");
    httpd_resp_sendstr_chunk(req, "</h1>");
    if (is_ap) httpd_resp_sendstr_chunk(req,
        "<p>Connect this device to your WiFi network.</p>");
    httpd_resp_sendstr_chunk(req,
        "<form method='POST' action='/wifi'>"
        "<label>Network (SSID)"
        "<input type='text' name='ssid' required maxlength='32'"
               " placeholder='Network name'></label>"
        "<label>Password"
        "<input type='password' name='pass' maxlength='63'"
               " placeholder='Leave blank for open network'></label>"
        "<button type='submit'>Save &amp; Restart</button>"
        "</form>"
        "<p class='note'>Device restarts and tries to connect."
        " Returns to AP setup mode if it fails.</p>");
    if (!is_ap)
        httpd_resp_sendstr_chunk(req,
            "<p><a href='/'>&#x2190; Back to price table</a></p>");
    httpd_resp_sendstr_chunk(req, "</body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t wifi_get_handler(httpd_req_t *req)
{
    send_wifi_form(req, s_ap_mode);
    return ESP_OK;
}

/* ── /wifi POST ──────────────────────────────────────────────────────────── */

static esp_err_t wifi_post_handler(httpd_req_t *req)
{
    char body[512] = {0};
    int  len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty"); return ESP_FAIL; }
    body[len] = '\0';

    char ssid[CRED_LEN] = {0}, pass[CRED_LEN] = {0};
    form_field(body, "ssid", ssid, sizeof(ssid));
    form_field(body, "pass", pass, sizeof(pass));

    if (ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
        return ESP_FAIL;
    }

    esp_err_t err = nvs_save_creds(ssid, pass);
    send_page_head(req, "ElereRelay \xe2\x80\x94 WiFi");
    if (err == ESP_OK) {
        char safe[CRED_LEN * 6] = {0};
        html_escape(ssid, safe, sizeof(safe));
        char html[640];
        snprintf(html, sizeof(html),
            "<h1>&#x2714; Saved!</h1>"
            "<p>Connecting to <b>%s</b>... Restarting.</p>"
            "<p class='note'>Returns to AP setup mode if connection fails.</p>"
            "</body></html>", safe);
        httpd_resp_sendstr_chunk(req, html);
        httpd_resp_sendstr_chunk(req, NULL);
        xTaskCreate(restart_task, "rst", 2048, NULL, 3, NULL);
    } else {
        httpd_resp_sendstr_chunk(req,
            "<h1>&#x274C; Error</h1><p>Failed to save credentials.</p>"
            "</body></html>");
        httpd_resp_sendstr_chunk(req, NULL);
    }
    return ESP_OK;
}

/* ── / (price table) ─────────────────────────────────────────────────────── */

static esp_err_t web_get_handler(httpd_req_t *req)
{
    char chunk[1024];

    send_page_head(req, "Elering Smart Relay");
    httpd_resp_sendstr_chunk(req, "<h1>&#x26A1; Elering Smart Relay</h1>");

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool        relay_on   = s_relay_on;
    int         count      = s_hour_count;
    time_t      last_fetch = s_last_fetch;
    bool        inv        = s_relay_inv;
    int         max_disp   = s_max_display;
    hour_slot_t local[MAX_SLOTS];
    memcpy(local, s_hours, count * sizeof(hour_slot_t));
    xSemaphoreGive(s_mutex);

    time_t now; struct tm ti;
    time(&now); localtime_r(&now, &ti);
    char time_str[32], fetch_str[32];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S %Z", &ti);
    if (last_fetch) {
        struct tm tf; localtime_r(&last_fetch, &tf);
        strftime(fetch_str, sizeof(fetch_str), "%H:%M:%S", &tf);
    } else {
        strlcpy(fetch_str, "never", sizeof(fetch_str));
    }

    char safe_ssid[CRED_LEN * 6] = {0};
    html_escape(s_wifi_ssid, safe_ssid, sizeof(safe_ssid));

    snprintf(chunk, sizeof(chunk),
        "<p>Time: <b>%s</b></p>"
        "<p>Relay: <span class='status %s'>%s</span>%s</p>"
        "<p class='meta'>IP: %s &nbsp;|&nbsp; WiFi: %s"
        " &nbsp;|&nbsp; Last fetch: %s"
        " &nbsp;|&nbsp; Window: %dh, cheap: %dh, fetch: %02d:00</p>",
        time_str,
        relay_on ? "on" : "off", relay_on ? "ON" : "OFF",
        inv ? " <span style='font-size:.8rem;color:#888'>(inverted)</span>" : "",
        s_ip, safe_ssid, fetch_str,
        s_hours_window, s_cheap_hours, s_fetch_hour);
    httpd_resp_sendstr_chunk(req, chunk);

    if (count == 0) {
        httpd_resp_sendstr_chunk(req,
            "<p><i>No price data yet &mdash; fetching&hellip;</i></p>");
    } else {
        /* Skip 15-min slots that have already ended */
        int start = 0;
        while (start < count && local[start].ts + 900 <= now) start++;
        int avail = count - start;
        int show  = avail < max_disp ? avail : max_disp;

        snprintf(chunk, sizeof(chunk),
            "<p class='meta'>Showing %d of %d future/current × 15-min slots.</p>"
            "<table><tr><th>Time (local)</th><th>Price (c/kWh)</th>"
            "<th>Relay</th></tr>", show, avail);
        httpd_resp_sendstr_chunk(req, chunk);

        time_t cur_hour = (now / 3600) * 3600;
        for (int i = start; i < start + show; i++) {
            struct tm th; localtime_r(&local[i].ts, &th);
            char hr[20]; strftime(hr, sizeof(hr), "%Y-%m-%d %H:%M", &th);
            bool is_cur   = (local[i].ts == cur_hour);
            bool is_cheap = local[i].cheap;
            bool ron      = is_cheap ^ inv;        /* physical relay state */
            float cprice  = local[i].price / 10.0f;
            int this_h = (int)(local[i].ts / 3600);
            int prev_h = (i > start)            ? (int)(local[i-1].ts / 3600) : -1;
            int next_h = (i < start + show - 1) ? (int)(local[i+1].ts / 3600) : -1;
            char row_cls[64];
            snprintf(row_cls, sizeof(row_cls), "hg %s%s%s%s",
                is_cheap ? "cheap" : "exp",
                is_cur ? " cur" : "",
                prev_h != this_h ? " hs" : "",
                next_h != this_h ? " he" : "");
            snprintf(chunk, sizeof(chunk),
                "<tr class='%s'>"
                "<td>%s</td><td>%.3f</td>"
                "<td class='%s'>%s</td></tr>",
                row_cls,
                hr, cprice,
                ron ? "ron" : "roff",
                ron ? "ON" : "OFF");
            httpd_resp_sendstr_chunk(req, chunk);
        }
        httpd_resp_sendstr_chunk(req, "</table>");
    }
    httpd_resp_sendstr_chunk(req, "</body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* ── /ota GET ────────────────────────────────────────────────────────────── */

static esp_err_t ota_get_handler(httpd_req_t *req)
{
    send_page_head(req, "ElereRelay \xe2\x80\x94 OTA Update");
    httpd_resp_sendstr_chunk(req, "<h1>&#x1F4E6; OTA Firmware Update</h1>");

    /* Current partition info */
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *update  = esp_ota_get_next_update_partition(NULL);
    const esp_app_desc_t  *desc    = esp_app_get_description();

    char info[512];
    snprintf(info, sizeof(info),
        "<p class='meta'>Running: <b>%s</b> &nbsp;|&nbsp; "
        "Version: <b>%s</b> &nbsp;|&nbsp; "
        "Built: <b>%s %s</b></p>"
        "<p class='meta'>Next update slot: <b>%s</b></p>",
        running ? running->label : "?",
        desc->version,
        desc->date, desc->time,
        update ? update->label : "none");
    httpd_resp_sendstr_chunk(req, info);

    httpd_resp_sendstr_chunk(req,
        "<p>Select a <code>.bin</code> firmware image built with"
        " <code>idf.py build</code> (<code>build/elering_relay.bin</code>).</p>"
        "<input type='file' id='fw' accept='.bin'>"
        "<p id='fsize' class='note'></p>"
        "<button id='btn' onclick='doOta()'>Upload &amp; Flash</button>"
        "<progress id='bar' value='0' max='100' style='display:none'></progress>"
        "<p id='status' class='ota-status'></p>"
        "<script>"
        "document.getElementById('fw').onchange=function(){"
          "var f=this.files[0];"
          "if(f)document.getElementById('fsize').textContent="
            "(f.size/1024).toFixed(1)+' KB';"
        "};"
        "function doOta(){"
          "var f=document.getElementById('fw').files[0];"
          "if(!f){alert('Select a .bin file first');return;}"
          "var btn=document.getElementById('btn'),"
              "bar=document.getElementById('bar'),"
              "st=document.getElementById('status');"
          "btn.disabled=true;"
          "bar.style.display='block';"
          "st.textContent='Starting upload...';"
          "var xhr=new XMLHttpRequest();"
          "xhr.open('POST','/ota');"
          "xhr.setRequestHeader('Content-Type','application/octet-stream');"
          "xhr.upload.onprogress=function(e){"
            "if(e.lengthComputable){"
              "var pct=Math.round(e.loaded/e.total*100);"
              "bar.value=pct;"
              "st.textContent='Uploading... '+pct+'%';"
            "}"
          "};"
          "xhr.onload=function(){"
            "bar.value=100;"
            "if(xhr.status===200){"
              "st.style.color='#1b5e20';"
              "st.textContent='\u2714 Done! Rebooting \u2014 reconnect in ~5 s';"
              "setTimeout(function(){window.location.href='/';},7000);"
            "}else{"
              "st.style.color='#b71c1c';"
              "st.textContent='\u2718 Failed: '+xhr.responseText;"
              "btn.disabled=false;"
            "}"
          "};"
          "xhr.onerror=function(){"
            "st.style.color='#b71c1c';"
            "st.textContent='\u2718 Upload error';"
            "btn.disabled=false;"
          "};"
          "xhr.send(f);"
        "}"
        "</script>"
        "</body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* ── /ota POST ───────────────────────────────────────────────────────────── */

static esp_err_t ota_post_handler(httpd_req_t *req)
{
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition found");
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    char *buf = malloc(2048);
    if (!buf) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    int written   = 0;
    bool failed   = false;

    while (remaining > 0) {
        int to_recv = remaining < 2048 ? remaining : 2048;
        int recv = httpd_req_recv(req, buf, to_recv);
        if (recv <= 0) {
            ESP_LOGE(TAG, "OTA recv error: %d (remaining %d)", recv, remaining);
            failed = true;
            break;
        }
        err = esp_ota_write(ota_handle, buf, recv);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            failed = true;
            break;
        }
        remaining -= recv;
        written   += recv;
    }
    free(buf);

    if (failed) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
        return ESP_FAIL;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA validation failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot partition failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA success — %d bytes written to %s — rebooting", written, update->label);
    httpd_resp_sendstr(req, "OK");
    xTaskCreate(restart_task, "ota_rst", 2048, NULL, 3, NULL);
    return ESP_OK;
}

/* ── Web server ──────────────────────────────────────────────────────────── */

static httpd_handle_t start_webserver(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.stack_size       = 8192;
    cfg.max_uri_handlers = 12;

    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server"); return NULL;
    }

    static const httpd_uri_t u_root_sta  = { "/",         HTTP_GET,  web_get_handler,      NULL };
    static const httpd_uri_t u_root_ap   = { "/",         HTTP_GET,  wifi_get_handler,     NULL };
    static const httpd_uri_t u_wifi_get  = { "/wifi",     HTTP_GET,  wifi_get_handler,     NULL };
    static const httpd_uri_t u_wifi_post = { "/wifi",     HTTP_POST, wifi_post_handler,    NULL };
    static const httpd_uri_t u_set_get   = { "/settings", HTTP_GET,  settings_get_handler, NULL };
    static const httpd_uri_t u_set_post  = { "/settings", HTTP_POST, settings_post_handler,NULL };
    static const httpd_uri_t u_ota_get   = { "/ota",      HTTP_GET,  ota_get_handler,      NULL };
    static const httpd_uri_t u_ota_post  = { "/ota",      HTTP_POST, ota_post_handler,     NULL };

    if (s_ap_mode) {
        httpd_register_uri_handler(srv, &u_root_ap);
    } else {
        httpd_register_uri_handler(srv, &u_root_sta);
        httpd_register_uri_handler(srv, &u_wifi_get);
        httpd_register_uri_handler(srv, &u_set_get);
        httpd_register_uri_handler(srv, &u_set_post);
        httpd_register_uri_handler(srv, &u_ota_get);
        httpd_register_uri_handler(srv, &u_ota_post);
    }
    httpd_register_uri_handler(srv, &u_wifi_post);

    ESP_LOGI(TAG, "HTTP server ready at http://%s/", s_ip);
    return srv;
}

/* ── Display update ──────────────────────────────────────────────────────── */

static void update_display(void)
{
    time_t now; time(&now);
    time_t cur_slot = (now / 900) * 900;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool relay_on  = s_relay_on;
    bool ap_mode   = s_ap_mode;
    int  count     = s_hour_count;
    hour_slot_t local[MAX_SLOTS];
    memcpy(local, s_hours, count * sizeof(hour_slot_t));
    xSemaphoreGive(s_mutex);

    /* Find index of current slot */
    int cur_idx = 0;
    for (int i = 0; i < count; i++) {
        if (local[i].ts == cur_slot) { cur_idx = i; break; }
    }

    /* Build display slots from current slot onward */
    disp_slot_t dslots[DISP_MAX_BARS];
    int dcount = 0;
    for (int i = cur_idx; i < count && dcount < DISP_MAX_BARS; i++, dcount++) {
        dslots[dcount].price_eur_mwh = local[i].price;
        dslots[dcount].is_cheap      = local[i].cheap;
    }

    display_update(relay_on, ap_mode, s_wifi_ssid,
                   now, dslots, dcount, 0,
                   s_cheap_hours, s_hours_window);
}

/* ── Controller task ─────────────────────────────────────────────────────── */

static void controller_task(void *arg)
{
    /* Fetch prices on startup */
    while (fetch_prices() != ESP_OK) {
        ESP_LOGW(TAG, "Initial fetch failed, retry in 30s");
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
    update_relay();
    update_display();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));   /* tick every minute */
        time_t now; time(&now);
        struct tm ti; localtime_r(&now, &ti);

        bool stale     = (s_hour_count == 0 || now - s_last_fetch > 25 * 3600);
        bool at_time   = (ti.tm_hour == s_fetch_hour);
        bool not_fresh = (now - s_last_fetch > 1800);   /* >30 min since last fetch */

        if (stale || (at_time && not_fresh)) {
            ESP_LOGI(TAG, "Refreshing prices (stale=%d at_time=%d)", stale, at_time);
            if (fetch_prices() != ESP_OK)
                ESP_LOGW(TAG, "Fetch failed, keeping old data");
        }
        update_relay();
        update_display();   /* refresh clock + price bar every minute */
    }
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

void app_main(void)
{
    ESP_LOGI(TAG, "ElereRelay booting (relay GPIO%d)", CONFIG_RELAY_GPIO);

    /* Display */
    display_init();
    display_status("Booting...", NULL);

    /* NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase()); ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Load runtime settings (before anything that uses them) */
    nvs_load_settings();
    ESP_LOGI(TAG, "Settings: window=%d cheap=%d inv=%d fetch_h=%d max_disp=%d "
             "ntp=%s tz=%s mqtt=%d host=%s port=%d",
             s_hours_window, s_cheap_hours, s_relay_inv, s_fetch_hour, s_max_display,
             s_ntp_server, s_tz_str, s_mqtt_enabled, s_mqtt_host, s_mqtt_port);

    s_mutex = xSemaphoreCreateMutex();

    /* Relay + RGB LED GPIOs — initialise OFF */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << RELAY_GPIO)
                      | (1ULL << LED_RED_GPIO)
                      | (1ULL << LED_GREEN_GPIO)
                      | (1ULL << LED_BLUE_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(RELAY_GPIO,    RELAY_OFF_LEVEL);
    gpio_set_level(LED_RED_GPIO,  LED_OFF);
    gpio_set_level(LED_GREEN_GPIO, LED_OFF);
    gpio_set_level(LED_BLUE_GPIO, LED_OFF);

    /* WiFi common init */
    s_wifi_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    /* WiFi credentials: NVS → Kconfig fallback */
    char ssid[CRED_LEN] = {0}, pass[CRED_LEN] = {0};
    if (nvs_load_creds(ssid, pass) != ESP_OK) {
        strlcpy(ssid, CONFIG_WIFI_SSID,     sizeof(ssid));
        strlcpy(pass, CONFIG_WIFI_PASSWORD, sizeof(pass));
        ESP_LOGI(TAG, "No NVS creds, using Kconfig defaults (SSID: %s)", ssid);
    } else {
        ESP_LOGI(TAG, "NVS creds loaded (SSID: %s)", ssid);
    }
    strlcpy(s_wifi_ssid, ssid, sizeof(s_wifi_ssid));

    char wifi_msg[72];
    snprintf(wifi_msg, sizeof(wifi_msg), "SSID: %s", ssid);
    display_status("Connecting to WiFi", wifi_msg);

    if (wifi_start_sta(ssid, pass)) {
        s_ap_mode = false;
        display_status("Syncing time...", s_ip);
        ntp_sync();
        mqtt_start();
        start_webserver();
        xTaskCreate(controller_task, "ctrl", 8192, NULL, 5, NULL);
    } else {
        s_ap_mode = true;
        ESP_LOGW(TAG, "STA failed — AP mode");
        wifi_start_ap();
        start_webserver();
        update_display();   /* show AP mode on screen */
    }
}
