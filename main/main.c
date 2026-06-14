/*
 * Elering Smart Relay — ESP32-C3
 *
 * Boot: read NVS creds → try STA → fallback AP "EleRelay-Setup"
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
#include "lwip/inet.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_sntp.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "mqtt_client.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_timer.h"
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

#define AP_SSID          "EleRelay-Setup"
#define NVS_NS           "elerelay"
/* WiFi credential keys */
#define NVS_KEY_SSID     "wifi_ssid"
#define NVS_KEY_PASS     "wifi_pass"
#define NVS_KEY_IP_MODE  "ip_mode"
#define NVS_KEY_IP_ADDR  "ip_addr"
#define NVS_KEY_IP_MASK  "ip_mask"
#define NVS_KEY_IP_GW    "ip_gw"
#define NVS_KEY_IP_DNS   "ip_dns"
/* Settings keys */
#define NVS_KEY_WINDOW   "h_window"
#define NVS_KEY_CHEAP    "cheap_h_n"
#define NVS_KEY_INV      "relay_inv"
#define NVS_KEY_FETCHH   "fetch_hour"
#define NVS_KEY_NTP_SRV  "ntp_server"
#define NVS_KEY_TZ       "tz_str"
#define NVS_KEY_MQTT_EN  "mqtt_en"
#define NVS_KEY_MQTT_HOST "mqtt_host"
#define NVS_KEY_MQTT_PORT "mqtt_port"
#define NVS_KEY_MQTT_TPRC "mqtt_topic_p"
#define NVS_KEY_MQTT_TRLY "mqtt_topic_r"
#define NVS_KEY_LANG      "lang"
#define NVS_KEY_MIN_RUN   "min_run_min"
#define NVS_KEY_AON_EN    "aon_en"
#define NVS_KEY_AON_LIM   "aon_lim"
#define NVS_KEY_AOFF_EN   "aoff_en"
#define NVS_KEY_AOFF_LIM  "aoff_lim"
#define NVS_KEY_BL        "bl_pct"
#define NVS_KEY_INET_CHK  "inet_chk_min"

/* Picks between English and Estonian at runtime */
#define T(en, et) (s_lang == LANG_ET ? (et) : (en))

#define CRED_LEN         64
#define STR_LEN          64
#define IP_STR_LEN       16

/* ── Runtime settings (loaded from NVS, defaults from Kconfig) ───────────── */

static int  s_hours_window    = CONFIG_HOURS_WINDOW;    /* 2-24               */
static int  s_cheap_hours     = CONFIG_CHEAP_HOURS;     /* 1 .. window-1      */
static int  s_lang            = LANG_EN;                /* LANG_EN / LANG_ET  */
static bool s_relay_inv       = false;                  /* invert relay logic */
static int  s_fetch_hour      = 23;                     /* 0-23               */
static int  s_min_run_minutes = CONFIG_MIN_RUN_MINUTES; /* 0 = disabled       */
static bool s_always_on_en    = false;                  /* force ON below limit */
static int  s_always_on_limit = 0;                      /* EUR/MWh (0 = 0 c/kWh) */
static bool s_always_off_en   = false;                  /* force OFF above limit */
static int  s_always_off_limit = 200;                   /* EUR/MWh (200 = 20 c/kWh) */
static int  s_backlight_pct   = 100;                    /* 0-100 % */
static int  s_inet_check_min  = 5;                      /* 0 = disabled */

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
#define WIFI_ASSOC_BIT     BIT2
static int s_retry_num = 0;

static SemaphoreHandle_t s_mutex;
static hour_slot_t       s_hours[MAX_SLOTS];
static int               s_hour_count    = 0;
static bool              s_relay_on      = false;
static time_t            s_relay_on_since = 0;  /* when relay last turned ON, 0 if OFF */
static time_t            s_last_fetch    = 0;
static bool              s_inet_ok       = true;
static time_t            s_last_inet_check = 0;
static time_t            s_last_slot_ts  = 0;  /* end of last fetched slot (ts + 900) */

typedef enum {
    RELAY_RSN_NO_DATA = 0,
    RELAY_RSN_SCHEDULE,
    RELAY_RSN_ALWAYS_ON,
    RELAY_RSN_MIN_RUN,
    RELAY_RSN_ALWAYS_OFF,
} relay_rsn_t;
static relay_rsn_t s_relay_reason = RELAY_RSN_NO_DATA;
static char              s_ip[20]     = "?.?.?.?";
static bool              s_ap_mode    = false;
static char              s_wifi_ssid[CRED_LEN] = {0};

static int  s_ip_mode    = 0;                  /* 0 = DHCP, 1 = static */
static char s_static_ip[IP_STR_LEN]   = "";
static char s_static_mask[IP_STR_LEN] = "255.255.255.0";
static char s_static_gw[IP_STR_LEN]   = "";
static char s_static_dns[IP_STR_LEN]  = "";

static char *s_http_buf = NULL;
static int   s_http_len = 0;

static esp_mqtt_client_handle_t s_mqtt_client = NULL;

/* ── Touch / screen state ────────────────────────────────────────────────── */
typedef enum { SCREEN_MAIN, SCREEN_CONFIG } screen_t;
static volatile screen_t s_screen    = SCREEN_MAIN;
static int               s_cfg_window   = 0;
static int               s_cfg_cheap    = 0;
static int               s_cfg_min_run  = 0;
static bool              s_cfg_aon_en   = false;
static bool              s_cfg_aoff_en  = false;
static int               s_cfg_aon_lim  = 0;
static int               s_cfg_aoff_lim = 200;
static int               s_cfg_bl_pct   = 100;
static int               s_cfg_page     = 0;

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
    if (nvs_get_u8(h, NVS_KEY_MQTT_EN, &v) == ESP_OK) s_mqtt_enabled = (bool)v;
    if (nvs_get_u8(h, NVS_KEY_LANG,    &v) == ESP_OK) s_lang         = v;
    if (nvs_get_u8(h, NVS_KEY_BL,      &v) == ESP_OK) s_backlight_pct = v;
    if (nvs_get_u8(h, NVS_KEY_IP_MODE, &v) == ESP_OK) s_ip_mode      = v;
    if (nvs_get_u8(h, NVS_KEY_INET_CHK, &v) == ESP_OK) s_inet_check_min = v;

    uint16_t port;
    if (nvs_get_u16(h, NVS_KEY_MQTT_PORT, &port) == ESP_OK) s_mqtt_port = port;

    uint16_t mrm;
    if (nvs_get_u16(h, NVS_KEY_MIN_RUN, &mrm) == ESP_OK) s_min_run_minutes = mrm;

    if (nvs_get_u8(h, NVS_KEY_AON_EN,  &v) == ESP_OK) s_always_on_en  = (bool)v;
    if (nvs_get_u8(h, NVS_KEY_AOFF_EN, &v) == ESP_OK) s_always_off_en = (bool)v;
    int16_t lim;
    if (nvs_get_i16(h, NVS_KEY_AON_LIM,  &lim) == ESP_OK) s_always_on_limit  = lim;
    if (nvs_get_i16(h, NVS_KEY_AOFF_LIM, &lim) == ESP_OK) s_always_off_limit = lim;

    size_t len;
    len = STR_LEN; nvs_get_str(h, NVS_KEY_NTP_SRV,  s_ntp_server,       &len);
    len = STR_LEN; nvs_get_str(h, NVS_KEY_TZ,        s_tz_str,           &len);
    len = STR_LEN; nvs_get_str(h, NVS_KEY_MQTT_HOST, s_mqtt_host,        &len);
    len = STR_LEN; nvs_get_str(h, NVS_KEY_MQTT_TPRC, s_mqtt_topic_price, &len);
    len = STR_LEN; nvs_get_str(h, NVS_KEY_MQTT_TRLY, s_mqtt_topic_relay, &len);

    len = IP_STR_LEN; nvs_get_str(h, NVS_KEY_IP_ADDR, s_static_ip,   &len);
    len = IP_STR_LEN; nvs_get_str(h, NVS_KEY_IP_MASK, s_static_mask, &len);
    len = IP_STR_LEN; nvs_get_str(h, NVS_KEY_IP_GW,   s_static_gw,   &len);
    len = IP_STR_LEN; nvs_get_str(h, NVS_KEY_IP_DNS,  s_static_dns,  &len);

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
    err |= nvs_set_u8(h,  NVS_KEY_MQTT_EN,   (uint8_t)s_mqtt_enabled);
    err |= nvs_set_u8(h,  NVS_KEY_LANG,      (uint8_t)s_lang);
    err |= nvs_set_u8(h,  NVS_KEY_BL,        (uint8_t)s_backlight_pct);
    err |= nvs_set_u8(h,  NVS_KEY_IP_MODE,   (uint8_t)s_ip_mode);
    err |= nvs_set_u8(h,  NVS_KEY_INET_CHK,  (uint8_t)s_inet_check_min);
    err |= nvs_set_u16(h, NVS_KEY_MQTT_PORT,  (uint16_t)s_mqtt_port);
    err |= nvs_set_u16(h, NVS_KEY_MIN_RUN,   (uint16_t)s_min_run_minutes);
    err |= nvs_set_u8(h,  NVS_KEY_AON_EN,    (uint8_t)s_always_on_en);
    err |= nvs_set_i16(h, NVS_KEY_AON_LIM,   (int16_t)s_always_on_limit);
    err |= nvs_set_u8(h,  NVS_KEY_AOFF_EN,   (uint8_t)s_always_off_en);
    err |= nvs_set_i16(h, NVS_KEY_AOFF_LIM,  (int16_t)s_always_off_limit);
    err |= nvs_set_str(h, NVS_KEY_NTP_SRV,   s_ntp_server);
    err |= nvs_set_str(h, NVS_KEY_TZ,        s_tz_str);
    err |= nvs_set_str(h, NVS_KEY_MQTT_HOST, s_mqtt_host);
    err |= nvs_set_str(h, NVS_KEY_MQTT_TPRC, s_mqtt_topic_price);
    err |= nvs_set_str(h, NVS_KEY_MQTT_TRLY, s_mqtt_topic_relay);
    err |= nvs_set_str(h, NVS_KEY_IP_ADDR,   s_static_ip);
    err |= nvs_set_str(h, NVS_KEY_IP_MASK,   s_static_mask);
    err |= nvs_set_str(h, NVS_KEY_IP_GW,     s_static_gw);
    err |= nvs_set_str(h, NVS_KEY_IP_DNS,    s_static_dns);
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
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        xEventGroupSetBits(s_wifi_eg, WIFI_ASSOC_BIT);
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

    if (s_ip_mode == 1 && s_static_ip[0]) {
        esp_netif_dhcpc_stop(sta);
        esp_netif_ip_info_t ip_info = {0};
        ip4_addr_t addr;
        if (ip4addr_aton(s_static_ip, &addr))   ip_info.ip.addr      = addr.addr;
        if (ip4addr_aton(s_static_mask, &addr)) ip_info.netmask.addr = addr.addr;
        if (ip4addr_aton(s_static_gw, &addr))   ip_info.gw.addr      = addr.addr;
        esp_netif_set_ip_info(sta, &ip_info);
        if (s_static_dns[0] && ip4addr_aton(s_static_dns, &addr)) {
            esp_netif_dns_info_t dns_info = {0};
            dns_info.ip.type     = ESP_IPADDR_TYPE_V4;
            dns_info.ip.u_addr.ip4.addr = addr.addr;
            esp_netif_set_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns_info);
        }
        ESP_LOGI(TAG, "Static IP: %s / %s gw %s dns %s",
                 s_static_ip, s_static_mask, s_static_gw, s_static_dns);
    }

    wifi_config_t wcfg = {0};
    strlcpy((char *)wcfg.sta.ssid,     ssid, sizeof(wcfg.sta.ssid));
    strlcpy((char *)wcfg.sta.password, pass, sizeof(wcfg.sta.password));
    if (strlen(pass) > 0) wcfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_eg, WIFI_ASSOC_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, portMAX_DELAY);
    if (bits & WIFI_FAIL_BIT) return false;

    display_status("Connecting to WiFi",
                    (s_ip_mode == 1) ? "Setting static IP..." : "Getting IP via DHCP...");

    bits = xEventGroupWaitBits(
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

/* Mark the cheapest slots as cheap=true, independently per hours-window.
 *
 * Without min_run: picks the cheapest cheap_n individual 15-min slots.
 * With min_run:    picks contiguous blocks of blk=ceil(min_run/15) slots so the
 *                  relay never activates for less than min_run minutes.
 *                  Number of blocks = max(1, cheap_n / blk). */
static void mark_cheap_hours(hour_slot_t *slots, int count, int min_run_minutes)
{
    if (count == 0) return;
    for (int i = 0; i < count; i++) slots[i].cheap = false;

    int    cheap_n   = s_cheap_hours * 4;
    int    win_secs  = (s_hours_window > 0 ? s_hours_window : 1) * 3600;
    int    blk       = (min_run_minutes > 15) ? (min_run_minutes + 14) / 15 : 0;
    time_t base      = slots[0].ts;

    int i = 0;
    while (i < count) {
        /* Find all slots belonging to the same window */
        int wn = (int)((slots[i].ts - base) / win_secs);
        int j  = i;
        while (j < count && (int)((slots[j].ts - base) / win_secs) == wn) j++;
        int wlen = j - i;

        if (blk > 1 && blk <= wlen) {
            /* Block mode: greedily pick cheapest non-overlapping runs of blk slots */
            int n_blocks = cheap_n / blk;
            if (n_blocks < 1) n_blocks = 1;

            bool used[MAX_SLOTS];
            memset(used, 0, sizeof(bool) * wlen);

            for (int b = 0; b < n_blocks; b++) {
                float best_sum = 1e18f;
                int   best_s   = -1;
                for (int s = i; s + blk <= j; s++) {
                    bool overlap = false;
                    for (int k = 0; k < blk; k++)
                        if (used[s - i + k]) { overlap = true; break; }
                    if (overlap) continue;
                    float sum = 0;
                    for (int k = 0; k < blk; k++) sum += slots[s + k].price;
                    if (sum < best_sum) { best_sum = sum; best_s = s; }
                }
                if (best_s < 0) break;
                for (int k = 0; k < blk; k++) {
                    slots[best_s + k].cheap = true;
                    used[best_s - i + k]    = true;
                }
            }
        } else {
            /* Original: mark cheapest cheap_n individual slots (insertion sort) */
            int idx[MAX_SLOTS];
            for (int k = 0; k < wlen; k++) idx[k] = i + k;
            for (int a = 1; a < wlen; a++) {
                int key = idx[a], b = a - 1;
                while (b >= 0 && slots[idx[b]].price > slots[key].price)
                    { idx[b+1] = idx[b]; b--; }
                idx[b+1] = key;
            }
            int n = wlen < cheap_n ? wlen : cheap_n;
            for (int k = 0; k < n; k++) slots[idx[k]].cheap = true;
        }

        i = j;
    }
}

/* ── Internet connectivity check ─────────────────────────────────────────── */

static bool inet_check(void)
{
    esp_http_client_config_t cfg = {
        .url               = "https://dashboard.elering.ee/",
        .method            = HTTP_METHOD_HEAD,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t err    = esp_http_client_perform(client);
    int       status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return err == ESP_OK && status >= 200 && status < 400;
}

static esp_err_t fetch_prices(void)
{
    time_t now; time(&now);
    time_t win_start = (now / 3600) * 3600;
    time_t win_end   = win_start + (MAX_SLOTS / 4) * 3600;

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
    mark_cheap_hours(s_hours, s_hour_count, s_min_run_minutes);
    s_last_fetch   = now;
    s_last_slot_ts = (nraw > 0) ? raw[nraw - 1].ts + 900 : 0;
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Parsed %d × 15-min slots", s_hour_count);
    return ESP_OK;
}

static void update_display(void);   /* forward declaration */
static void compute_effective_relay_states(const hour_slot_t *, int, bool, int,
                                           bool, int, bool, int,
                                           bool, time_t, bool *); /* forward */

/* ── Relay ───────────────────────────────────────────────────────────────── */

static void update_relay(void)
{
    time_t now; time(&now);
    time_t cur_slot = (now / 900) * 900;    /* round down to 15-min boundary */
    bool  is_cheap       = false;
    bool  found_slot     = false;
    float cur_price      = 0.0f;
    float raw_price_mwh  = 0.0f;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_hour_count; i++) {
        if (s_hours[i].ts == cur_slot) {
            is_cheap      = s_hours[i].cheap;
            cur_price     = s_hours[i].price / 10.0f;   /* EUR/MWh → c/kWh */
            raw_price_mwh = s_hours[i].price;
            found_slot    = true;
            break;
        }
    }
    bool on;
    relay_rsn_t reason;

    if (!found_slot) {
        on = false;
        reason = RELAY_RSN_NO_DATA;
    } else {
        on = is_cheap ^ s_relay_inv;
        reason = RELAY_RSN_SCHEDULE;

        if (s_always_on_en && raw_price_mwh <= (float)s_always_on_limit) {
            on = true;
            reason = RELAY_RSN_ALWAYS_ON;
        }

        if (!on && s_min_run_minutes > 0 && s_relay_on && s_relay_on_since > 0) {
            if (now - s_relay_on_since < (time_t)s_min_run_minutes * 60) {
                on = true;
                reason = RELAY_RSN_MIN_RUN;
            }
        }

        /* Always OFF is highest priority */
        if (s_always_off_en && raw_price_mwh >= (float)s_always_off_limit) {
            on = false;
            reason = RELAY_RSN_ALWAYS_OFF;
        }
    }

    /* Track when relay transitions to ON */
    if (on && !s_relay_on)
        s_relay_on_since = now;
    else if (!on)
        s_relay_on_since = 0;

    s_relay_on     = on;
    s_relay_reason = reason;
    xSemaphoreGive(s_mutex);

    gpio_set_level(RELAY_GPIO, on ? RELAY_ON_LEVEL : RELAY_OFF_LEVEL);

    /* RGB LED: green = cheap, red = expensive, blue always off */
    gpio_set_level(LED_GREEN_GPIO, is_cheap ? LED_ON : LED_OFF);
    gpio_set_level(LED_RED_GPIO,   is_cheap ? LED_OFF : LED_ON);
    gpio_set_level(LED_BLUE_GPIO,  LED_OFF);

    ESP_LOGI(TAG, "Relay → %s (hour %s, inv=%d)",
             on ? "ON" : "OFF", is_cheap ? "cheap" : "expensive", s_relay_inv);

    /* MQTT publish relay state and current price */
    mqtt_publish(s_mqtt_topic_relay, on ? "1" : "0");
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

/* Retry STA connection every 5 minutes when stuck in AP mode.
 * Only started when NVS credentials exist, so first-boot setup is unaffected. */
static void ap_reconnect_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5 * 60 * 1000));
        ESP_LOGI(TAG, "AP mode: retrying STA connection");
        esp_restart();
    }
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
    "tr.win-sep td{background:#e8eaf6;color:#283593;font-weight:600;"
                  "font-size:.82rem;text-align:left;border-top:3px solid #5c6bc0;"
                  "border-bottom:2px solid #9fa8da}"
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
    char buf[512];
    httpd_resp_set_type(req, "text/html; charset=UTF-8");
    snprintf(buf, sizeof(buf),
        "<!DOCTYPE html><html lang='%s'><head>"
        "<meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<link rel='icon' href=\"data:image/svg+xml,"
        "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'>"
        "<text y='.9em' font-size='90'>&#x26A1;</text></svg>\">",
        T("en", "et"));
    httpd_resp_sendstr_chunk(req, buf);
    snprintf(buf, sizeof(buf), "<title>%s</title>", title);
    httpd_resp_sendstr_chunk(req, buf);
    httpd_resp_sendstr_chunk(req, PAGE_CSS);
    snprintf(buf, sizeof(buf),
        "</head><body><nav>"
        "<a href='/'>&#x26A1; %s</a>"
        "<a href='/relay'>&#x1F4CA; %s</a>"
        "<a href='/settings'>&#x2699;&#xFE0F; %s</a>"
        "<a href='/mqtt'>&#x1F4E1; MQTT</a>"
        "<a href='/wifi'>&#x1F4F6; %s</a>"
        "<a href='/ota'>&#x1F4E6; OTA</a>"
        "</nav>",
        T("Prices", "Hinnad"),
        T("Relay Settings", "Relee Seaded"),
        T("Settings", "Seaded"),
        T("Network", "V&otilde;rk"));
    httpd_resp_sendstr_chunk(req, buf);
}

/* ── /relay GET ──────────────────────────────────────────────────────────── */

static esp_err_t relay_get_handler(httpd_req_t *req)
{
    send_page_head(req, T("EleRelay \xe2\x80\x94 Relay Settings", "EleRelay \xe2\x80\x94 Relee Seaded"));
    char chunk[512];
    snprintf(chunk, sizeof(chunk), "<h1>&#x1F4CA; %s</h1>"
        "<form method='POST' action='/relay'>", T("Relay Settings", "Relee Seaded"));
    httpd_resp_sendstr_chunk(req, chunk);

    /* ── Relay / price settings ── */
    snprintf(chunk, sizeof(chunk), "<h2>%s</h2>", T("Relay &amp; Price", "Relee &amp; Hind"));
    httpd_resp_sendstr_chunk(req, chunk);

    /* Hours window dropdown */
    snprintf(chunk, sizeof(chunk), "<label>%s<select name='window'>",
             T("Hours window", "Ajaaken"));
    httpd_resp_sendstr_chunk(req, chunk);
    for (int i = 2; i <= 24; i++) {
        snprintf(chunk, sizeof(chunk), "<option value='%d'%s>%d %s</option>",
                 i, i == s_hours_window ? " selected" : "", i, T("hours", "tundi"));
        httpd_resp_sendstr_chunk(req, chunk);
    }
    httpd_resp_sendstr_chunk(req, "</select></label>");

    /* Cheap hours dropdown */
    snprintf(chunk, sizeof(chunk), "<label>%s<select name='cheap'>",
             T("Cheap hours", "Odavad tunnid"));
    httpd_resp_sendstr_chunk(req, chunk);
    for (int i = 1; i < s_hours_window; i++) {
        snprintf(chunk, sizeof(chunk), "<option value='%d'%s>%d %s</option>",
                 i, i == s_cheap_hours ? " selected" : "", i, T("hours", "tundi"));
        httpd_resp_sendstr_chunk(req, chunk);
    }
    httpd_resp_sendstr_chunk(req, "</select></label>");

    /* Minimum continuous run minutes */
    {
        char buf[600];
        snprintf(buf, sizeof(buf),
            "<label>%s"
            "<input type='number' name='min_run' min='0' max='480' value='%d'>"
            "</label>"
            "<p class='note'>%s</p>",
            T("Minimum continuous run (minutes, 0 = disabled)",
              "Minimaalne j&auml;rjestikune k&auml;itusaeg (minutit, 0 = v&auml;lja l&uuml;litatud)"),
            s_min_run_minutes,
            T("Once the relay turns ON it stays ON for at least this many minutes."
              " Useful for devices that need a full run cycle (e.g. 60 min = 1 hour).",
              "Kui relee l&auml;heb SISSE, j&auml;&auml;b see sisselülitatuks v&auml;hemalt nii kauaks."
              " Kasulik seadmetele, mis vajavad t&auml;ielikku t&ouml;&ouml;tsüklit (nt 60 min = 1 tund)."));
        httpd_resp_sendstr_chunk(req, buf);
    }

    /* Always ON / OFF overrides */
    snprintf(chunk, sizeof(chunk), "<h2>%s</h2>",
        T("Override: Always ON / Always OFF", "Limiidid: Alati SEES / V&auml;ljas"));
    httpd_resp_sendstr_chunk(req, chunk);

    {
        char buf[640];
        snprintf(buf, sizeof(buf),
            "<div class='chk'>"
            "<input type='checkbox' name='aon_en' id='aon_en'%s>"
            "<label for='aon_en' style='margin:0;color:#222'>%s</label>"
            "</div>"
            "<input type='text' inputmode='decimal' name='aon_lim'"
            " style='width:100%%;box-sizing:border-box;padding:7px;margin-top:4px;"
            "border:1px solid #ccc;border-radius:6px;font-size:1rem'"
            " value='%.1f'>",
            s_always_on_en ? " checked" : "",
            T("Always ON when price is at or below (c/kWh):",
              "Alati SEES kui hind on kuni (s/kWh):"),
            s_always_on_limit / 10.0f);
        httpd_resp_sendstr_chunk(req, buf);
    }
    snprintf(chunk, sizeof(chunk), "<p class='note'>%s</p>",
        T("Force relay ON whenever price &le; this value, ignoring the schedule.",
          "Sunnib relee SISSE kui hind &le; v&auml;&auml;rtus, ignoreerides graafiku."));
    httpd_resp_sendstr_chunk(req, chunk);

    {
        char buf[640];
        snprintf(buf, sizeof(buf),
            "<div class='chk'>"
            "<input type='checkbox' name='aoff_en' id='aoff_en'%s>"
            "<label for='aoff_en' style='margin:0;color:#222'>%s</label>"
            "</div>"
            "<input type='text' inputmode='decimal' name='aoff_lim'"
            " style='width:100%%;box-sizing:border-box;padding:7px;margin-top:4px;"
            "border:1px solid #ccc;border-radius:6px;font-size:1rem'"
            " value='%.1f'>",
            s_always_off_en ? " checked" : "",
            T("Always OFF when price is at or above (c/kWh):",
              "Alati V&auml;ljas kui hind on v&auml;hemalt (s/kWh):"),
            s_always_off_limit / 10.0f);
        httpd_resp_sendstr_chunk(req, buf);
    }
    snprintf(chunk, sizeof(chunk), "<p class='note'>%s</p>",
        T("Force relay OFF whenever price &ge; this value. Takes priority over Always ON and min-run.",
          "Sunnib relee V&auml;LJA kui hind &ge; v&auml;&auml;rtus. Prioriteet Always ON ja min-k&auml;ituse ees."));
    httpd_resp_sendstr_chunk(req, chunk);

    /* Relay inverted checkbox */
    {
        char buf[320];
        snprintf(buf, sizeof(buf),
            "<div class='chk'>"
            "<input type='checkbox' name='inv' id='inv'%s>"
            "<label for='inv' style='margin:0;color:#222'>%s</label>"
            "</div>",
            s_relay_inv ? " checked" : "",
            T("Inverted &mdash; ON during expensive hours",
              "P&ouml;&ouml;ratud &mdash; SEES kallite tundide ajal"));
        httpd_resp_sendstr_chunk(req, buf);
    }

    /* Fetch hour dropdown */
    snprintf(chunk, sizeof(chunk), "<label>%s<select name='fetch_h'>",
             T("Fetch prices at (daily)", "Laadi hinnad (iga p&auml;ev)"));
    httpd_resp_sendstr_chunk(req, chunk);
    for (int h = 0; h < 24; h++) {
        snprintf(chunk, sizeof(chunk), "<option value='%d'%s>%02d:00</option>",
                 h, h == s_fetch_hour ? " selected" : "", h);
        httpd_resp_sendstr_chunk(req, chunk);
    }
    httpd_resp_sendstr_chunk(req, "</select></label>");
    snprintf(chunk, sizeof(chunk), "<p class='note'>%s</p>",
        T("Elering publishes next-day prices around 14:00 EET."
          " Fetching at 23:00 ensures a full set of tomorrow&rsquo;s prices.",
          "Elering avaldab j&auml;rgmise p&auml;eva hinnad umbes 14:00 EET."
          " Laadimine kell 23:00 tagab t&auml;ieliku hinnakomplekti."));
    httpd_resp_sendstr_chunk(req, chunk);

    snprintf(chunk, sizeof(chunk),
        "<button type='submit'>%s</button>"
        "</form></body></html>",
        T("Save settings", "Salvesta seaded"));
    httpd_resp_sendstr_chunk(req, chunk);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* ── /settings GET ───────────────────────────────────────────────────────── */

static esp_err_t settings_get_handler(httpd_req_t *req)
{
    send_page_head(req, T("EleRelay \xe2\x80\x94 Settings", "EleRelay \xe2\x80\x94 Seaded"));
    char chunk[512];
    snprintf(chunk, sizeof(chunk), "<h1>&#x2699;&#xFE0F; %s</h1>"
        "<form method='POST' action='/settings'>", T("Settings", "Seaded"));
    httpd_resp_sendstr_chunk(req, chunk);

    /* ── Time settings ── */
    snprintf(chunk, sizeof(chunk), "<h2>%s</h2>", T("Time", "Aeg"));
    httpd_resp_sendstr_chunk(req, chunk);

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
            "<label>%s"
            "<input type='text' name='tz_str' maxlength='%d' value='%s'>"
            "</label>"
            "<p class='note'>%s: "
            "<code>EET-2EEST,M3.5.0/3,M10.5.0/4</code> (Estonia), "
            "<code>UTC0</code>, "
            "<code>CET-1CEST,M3.5.0,M10.5.0/3</code> (Central Europe)</p>",
            T("Timezone (POSIX TZ string)", "Ajev&ouml;&ouml;nd (POSIX)"),
            STR_LEN - 1, safe,
            T("Examples", "N&auml;ited"));
        httpd_resp_sendstr_chunk(req, buf);
    }

    /* ── Diagnostics ── */
    snprintf(chunk, sizeof(chunk), "<h2>%s</h2>", T("Diagnostics", "Diagnostika"));
    httpd_resp_sendstr_chunk(req, chunk);
    {
        char buf[512];
        snprintf(buf, sizeof(buf),
            "<label>%s"
            "<input type='number' name='inet_chk' min='0' max='60' value='%d'>"
            "</label>"
            "<p class='note'>%s</p>",
            T("Internet check interval (minutes, 0 = disabled)",
              "Interneti kontrolli intervall (minutit, 0 = v&auml;lja l&uuml;litatud)"),
            s_inet_check_min,
            T("Periodically checks internet connectivity. Shows a warning on the LCD if unreachable.",
              "Kontrollib perioodiliselt interneti&uuml;hendust. Kuvab LCD-l hoiatuse, kui &uuml;hendus puudub."));
        httpd_resp_sendstr_chunk(req, buf);
    }

    /* ── Language / Keel ── */
    httpd_resp_sendstr_chunk(req, "<h2>Keel / Language</h2>");
    snprintf(chunk, sizeof(chunk),
        "<label>Keel / Language"
        "<select name='lang'>"
        "<option value='0'%s>English</option>"
        "<option value='1'%s>Eesti</option>"
        "</select></label>",
        s_lang == LANG_EN ? " selected" : "",
        s_lang == LANG_ET ? " selected" : "");
    httpd_resp_sendstr_chunk(req, chunk);

    snprintf(chunk, sizeof(chunk),
        "<button type='submit'>%s</button>"
        "</form></body></html>",
        T("Save settings", "Salvesta seaded"));
    httpd_resp_sendstr_chunk(req, chunk);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* ── /relay POST ─────────────────────────────────────────────────────────── */

static esp_err_t relay_post_handler(httpd_req_t *req)
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

    form_field(body, "min_run", val, sizeof(val));
    int min_run = clamp(atoi(val), 0, 480);

    bool aon_en = (strstr(body, "aon_en=on") || strstr(body, "aon_en=1"));
    form_field(body, "aon_lim", val, sizeof(val));
    for (char *p = val; *p; p++) if (*p == ',') *p = '.';  /* locale-safe */
    int aon_lim = (int)(atof(val) * 10.0f);
    aon_lim = aon_lim < -1000 ? -1000 : aon_lim > 10000 ? 10000 : aon_lim;

    bool aoff_en = (strstr(body, "aoff_en=on") || strstr(body, "aoff_en=1"));
    form_field(body, "aoff_lim", val, sizeof(val));
    for (char *p = val; *p; p++) if (*p == ',') *p = '.';  /* locale-safe */
    int aoff_lim = (int)(atof(val) * 10.0f);
    aoff_lim = aoff_lim < -1000 ? -1000 : aoff_lim > 10000 ? 10000 : aoff_lim;

    /* Apply immediately */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_hours_window = window;
    s_cheap_hours  = cheap;
    s_relay_inv    = inv;
    s_fetch_hour   = fetch_h;
    s_min_run_minutes  = min_run;
    s_always_on_en     = aon_en;
    s_always_on_limit  = aon_lim;
    s_always_off_en    = aoff_en;
    s_always_off_limit = aoff_lim;
    mark_cheap_hours(s_hours, s_hour_count, s_min_run_minutes);   /* recompute with new cheap count */
    xSemaphoreGive(s_mutex);

    update_relay();          /* reflect new inversion + cheap hours instantly */
    nvs_save_settings();
    update_display();        /* reflect new settings on LCD immediately */

    ESP_LOGI(TAG, "Relay settings updated: window=%d cheap=%d inv=%d fetch_h=%d "
             "min_run=%d aon=%d@%d aoff=%d@%d",
             window, cheap, inv, fetch_h, min_run,
             aon_en, aon_lim, aoff_en, aoff_lim);

    send_page_head(req, T("EleRelay \xe2\x80\x94 Relay Settings", "EleRelay \xe2\x80\x94 Relee Seaded"));
    char chunk[1280];
    snprintf(chunk, sizeof(chunk),
        "<h1>%s</h1>"
        "<p>&#x2714; %s</p>"
        "<ul style='font-size:.9rem;line-height:1.8'>"
        "<li>%s <b>%d h</b></li>"
        "<li>%s <b>%d</b></li>"
        "<li>%s <b>%d min</b></li>"
        "<li>%s <b>%s</b></li>"
        "<li>%s <b>%02d:00</b></li>"
        "<li>%s <b>%s</b> (%.1f c/kWh)</li>"
        "<li>%s <b>%s</b> (%.1f c/kWh)</li>"
        "</ul>"
        "<p><a href='/relay'>%s</a>"
        " &nbsp; <a href='/'>%s</a></p>"
        "</body></html>",
        T("Relay Settings", "Relee Seaded"),
        T("Saved &amp; applied.", "Salvestatud &amp; rakendatud."),
        T("Window:", "Aken:"), window,
        T("Cheap hours:", "Odavad tunnid:"), cheap,
        T("Min. continuous run:", "Min. j&auml;rjestikune k&auml;itus:"), min_run,
        T("Relay inverted:", "Relee p&ouml;&ouml;ratud:"), inv ? T("yes","jah") : T("no","ei"),
        T("Fetch at:", "Laadimine:"), fetch_h,
        T("Always ON &le;:", "Alati SEES &le;:"),
        aon_en ? T("on","sees") : T("off","v&auml;ljas"), aon_lim / 10.0f,
        T("Always OFF &ge;:", "Alati V&auml;ljas &ge;:"),
        aoff_en ? T("on","sees") : T("off","v&auml;ljas"), aoff_lim / 10.0f,
        T("Back to relay settings", "Tagasi relee seadetesse"),
        T("Price table", "Hinnad"));
    httpd_resp_sendstr_chunk(req, chunk);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* ── /settings POST ──────────────────────────────────────────────────────── */

static esp_err_t settings_post_handler(httpd_req_t *req)
{
    char body[512] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty"); return ESP_FAIL; }
    body[len] = '\0';

    char val[STR_LEN];

    /* Time settings */
    char ntp_server[STR_LEN] = {0};
    form_field(body, "ntp_server", ntp_server, sizeof(ntp_server));
    if (ntp_server[0] == '\0') strlcpy(ntp_server, "pool.ntp.org", sizeof(ntp_server));

    char tz_str[STR_LEN] = {0};
    form_field(body, "tz_str", tz_str, sizeof(tz_str));
    if (tz_str[0] == '\0') strlcpy(tz_str, "UTC0", sizeof(tz_str));

    form_field(body, "lang", val, sizeof(val));
    int new_lang = clamp(atoi(val), 0, 1);

    form_field(body, "inet_chk", val, sizeof(val));
    int inet_chk = clamp(atoi(val), 0, 60);

    /* Apply immediately */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strlcpy(s_ntp_server,       ntp_server,  sizeof(s_ntp_server));
    strlcpy(s_tz_str,           tz_str,      sizeof(s_tz_str));
    s_lang = new_lang;
    s_inet_check_min = inet_chk;
    xSemaphoreGive(s_mutex);
    display_set_lang(new_lang);

    /* Apply timezone immediately */
    setenv("TZ", s_tz_str, 1);
    tzset();

    nvs_save_settings();
    update_display();        /* reflect new language on LCD immediately */

    ESP_LOGI(TAG, "Settings updated: ntp=%s tz=%s lang=%d inet_chk=%d",
             ntp_server, tz_str, new_lang, inet_chk);

    send_page_head(req, T("EleRelay \xe2\x80\x94 Settings", "EleRelay \xe2\x80\x94 Seaded"));
    char chunk[768];
    snprintf(chunk, sizeof(chunk),
        "<h1>%s</h1>"
        "<p>&#x2714; %s</p>"
        "<ul style='font-size:.9rem;line-height:1.8'>"
        "<li>NTP: <b>%s</b></li>"
        "<li>TZ: <b>%s</b></li>"
        "</ul>"
        "<p><a href='/settings'>%s</a>"
        " &nbsp; <a href='/'>%s</a></p>"
        "</body></html>",
        T("Settings", "Seaded"),
        T("Saved &amp; applied.", "Salvestatud &amp; rakendatud."),
        ntp_server, tz_str,
        T("Back to settings", "Tagasi seadetesse"),
        T("Price table", "Hinnad"));
    httpd_resp_sendstr_chunk(req, chunk);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* ── /mqtt GET ───────────────────────────────────────────────────────────── */

static esp_err_t mqtt_get_handler(httpd_req_t *req)
{
    send_page_head(req, "EleRelay \xe2\x80\x94 MQTT");
    char chunk[512];
    snprintf(chunk, sizeof(chunk), "<h1>&#x1F4E1; MQTT</h1>"
        "<form method='POST' action='/mqtt'>");
    httpd_resp_sendstr_chunk(req, chunk);

    /* MQTT enabled checkbox */
    {
        char buf[320];
        snprintf(buf, sizeof(buf),
            "<div class='chk'>"
            "<input type='checkbox' name='mqtt_en' id='mqtt_en'%s>"
            "<label for='mqtt_en' style='margin:0;color:#222'>%s</label>"
            "</div>",
            s_mqtt_enabled ? " checked" : "",
            T("Enable MQTT publishing", "Luba MQTT avaldamine"));
        httpd_resp_sendstr_chunk(req, buf);
    }

    /* MQTT server */
    {
        char safe[STR_LEN * 6] = {0};
        html_escape(s_mqtt_host, safe, sizeof(safe));
        char buf[640];
        snprintf(buf, sizeof(buf),
            "<label>%s"
            "<input type='text' name='mqtt_host' maxlength='%d' value='%s'"
            " placeholder='e.g. 192.168.1.10 or broker.local'>"
            "</label>",
            T("MQTT server hostname / IP", "MQTT serveri aadress / IP"),
            STR_LEN - 1, safe);
        httpd_resp_sendstr_chunk(req, buf);
    }

    /* MQTT port */
    {
        char buf[192];
        snprintf(buf, sizeof(buf),
            "<label>%s"
            "<input type='number' name='mqtt_port' min='1' max='65535' value='%d'>"
            "</label>",
            T("MQTT port", "MQTT port"), s_mqtt_port);
        httpd_resp_sendstr_chunk(req, buf);
    }

    /* MQTT price topic */
    {
        char safe[STR_LEN * 6] = {0};
        html_escape(s_mqtt_topic_price, safe, sizeof(safe));
        char buf[512];
        snprintf(buf, sizeof(buf),
            "<label>%s"
            "<input type='text' name='mqtt_topic_p' maxlength='%d' value='%s'>"
            "</label>",
            T("Price topic", "Hinna teema"), STR_LEN - 1, safe);
        httpd_resp_sendstr_chunk(req, buf);
    }

    /* MQTT relay topic */
    {
        char safe[STR_LEN * 6] = {0};
        html_escape(s_mqtt_topic_relay, safe, sizeof(safe));
        char buf[640];
        snprintf(buf, sizeof(buf),
            "<label>%s"
            "<input type='text' name='mqtt_topic_r' maxlength='%d' value='%s'>"
            "</label>"
            "<p class='note'>%s</p>",
            T("Relay state topic", "Relee oleku teema"), STR_LEN - 1, safe,
            T("Publishes price in c/kWh and relay state"
              " (<code>ON</code>/<code>OFF</code>) with retain=1 on every relay update.",
              "Avaldab hinna s/kWh ja relee oleku"
              " (<code>ON</code>/<code>OFF</code>) retain=1 iga relee muutuse korral."));
        httpd_resp_sendstr_chunk(req, buf);
    }

    snprintf(chunk, sizeof(chunk),
        "<button type='submit'>%s</button>"
        "</form></body></html>",
        T("Save settings", "Salvesta seaded"));
    httpd_resp_sendstr_chunk(req, chunk);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* ── /mqtt POST ──────────────────────────────────────────────────────────── */

static esp_err_t mqtt_post_handler(httpd_req_t *req)
{
    char body[640] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty"); return ESP_FAIL; }
    body[len] = '\0';

    char val[STR_LEN];

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

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_mqtt_enabled = mqtt_en;
    strlcpy(s_mqtt_host,        mqtt_host,    sizeof(s_mqtt_host));
    s_mqtt_port    = mqtt_port;
    strlcpy(s_mqtt_topic_price, mqtt_topic_p, sizeof(s_mqtt_topic_price));
    strlcpy(s_mqtt_topic_relay, mqtt_topic_r, sizeof(s_mqtt_topic_relay));
    xSemaphoreGive(s_mutex);

    nvs_save_settings();
    mqtt_start();   /* restart MQTT client with new settings */

    ESP_LOGI(TAG, "MQTT settings updated: en=%d host=%s port=%d tp=%s tr=%s",
             mqtt_en, mqtt_host, mqtt_port, mqtt_topic_p, mqtt_topic_r);

    send_page_head(req, "EleRelay \xe2\x80\x94 MQTT");
    char chunk[512];
    snprintf(chunk, sizeof(chunk),
        "<h1>MQTT</h1>"
        "<p>&#x2714; %s</p>"
        "<ul style='font-size:.9rem;line-height:1.8'>"
        "<li>MQTT: <b>%s</b>%s</li>"
        "</ul>"
        "<p><a href='/mqtt'>%s</a>"
        " &nbsp; <a href='/'>%s</a></p>"
        "</body></html>",
        T("Saved &amp; applied.", "Salvestatud &amp; rakendatud."),
        mqtt_en ? T("enabled","lubatud") : T("disabled","keelatud"),
        mqtt_en && mqtt_host[0] ? T(" \xe2\x80\x94 connecting...", " \xe2\x80\x94 \xc3\xbchendun...") : "",
        T("Back to MQTT settings", "Tagasi MQTT seadetesse"),
        T("Price table", "Hinnad"));
    httpd_resp_sendstr_chunk(req, chunk);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* ── /wifi GET ───────────────────────────────────────────────────────────── */

static void send_wifi_form(httpd_req_t *req, bool is_ap)
{
    send_page_head(req, T("EleRelay \xe2\x80\x94 Network", "EleRelay \xe2\x80\x94 V&otilde;rk"));
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "<h1>&#x1F4F6; %s</h1>%s"
        "<form method='POST' action='/wifi'>"
        "<label>%s<input type='text' name='ssid' required maxlength='32'"
        " value='%s' placeholder='%s'></label>"
        "<label>%s<input type='password' name='pass' maxlength='63'"
        " placeholder='%s'></label>"
        "<h2>%s</h2>"
        "<div class='chk'><input type='radio' name='ip_mode' value='dhcp' id='ipd'%s"
        " onchange='elIpToggle()'><label for='ipd' style='margin:0'>%s</label></div>"
        "<div class='chk'><input type='radio' name='ip_mode' value='static' id='ips'%s"
        " onchange='elIpToggle()'><label for='ips' style='margin:0'>%s</label></div>",
        is_ap ? T("First Setup", "Esmane seadistus") : T("Network Settings", "V&otilde;rgu seaded"),
        is_ap ? T("<p>Connect this device to your WiFi network.</p>",
                  "<p>&#xDC;hendage seade WiFi v&otilde;rguga.</p>") : "",
        T("Network (SSID)", "V&otilde;rk (SSID)"),
        s_wifi_ssid,
        T("Network name", "V&otilde;rgu nimi"),
        T("Password", "Parool"),
        T("Leave blank for open network", "J&auml;ta t&uuml;hjaks avatud v&otilde;rgu puhul"),
        T("IP Configuration", "IP seadistus"),
        s_ip_mode == 0 ? " checked" : "",
        T("DHCP (automatic)", "DHCP (automaatne)"),
        s_ip_mode == 1 ? " checked" : "",
        T("Static IP", "Staatiline IP"));
    httpd_resp_sendstr_chunk(req, buf);

    snprintf(buf, sizeof(buf),
        "<div id='ipfields'>"
        "<label>%s<input type='text' name='ip_addr' maxlength='15' value='%s'"
        " placeholder='192.168.1.50'></label>"
        "<label>%s<input type='text' name='ip_mask' maxlength='15' value='%s'"
        " placeholder='255.255.255.0'></label>"
        "<label>%s<input type='text' name='ip_gw' maxlength='15' value='%s'"
        " placeholder='192.168.1.1'></label>"
        "<label>%s<input type='text' name='ip_dns' maxlength='15' value='%s'"
        " placeholder='192.168.1.1'></label>"
        "</div>"
        "<script>function elIpToggle(){"
        "document.getElementById('ipfields').style.display="
        "document.getElementById('ips').checked?'block':'none';}"
        "elIpToggle();</script>"
        "<button type='submit'>%s</button>"
        "</form>"
        "<p class='note'>%s</p>",
        T("IP Address", "IP aadress"), s_static_ip,
        T("Subnet Mask", "Alamv&otilde;rgu mask"), s_static_mask,
        T("Gateway", "L&uuml;&uuml;s"), s_static_gw,
        T("DNS", "DNS"), s_static_dns,
        T("Save &amp; Restart", "Salvesta &amp; Taask&auml;ivita"),
        T("Device restarts and tries to connect. Returns to AP setup mode if it fails.",
          "Seade taask&auml;ivitub ja proovib &uuml;henduda. Kui ei &otilde;nnestu, l&auml;heb AP-re&zcaron;iimi."));
    httpd_resp_sendstr_chunk(req, buf);
    if (!is_ap) {
        char back[128];
        snprintf(back, sizeof(back), "<p><a href='/'>&#x2190; %s</a></p>",
                 T("Back to price table", "Tagasi hindade lehele"));
        httpd_resp_sendstr_chunk(req, back);
    }
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
    char body[768] = {0};
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

    char ip_mode[8] = {0};
    char ip_addr[IP_STR_LEN] = {0}, ip_mask[IP_STR_LEN] = {0};
    char ip_gw[IP_STR_LEN]   = {0}, ip_dns[IP_STR_LEN]  = {0};
    form_field(body, "ip_mode", ip_mode, sizeof(ip_mode));
    form_field(body, "ip_addr", ip_addr, sizeof(ip_addr));
    form_field(body, "ip_mask", ip_mask, sizeof(ip_mask));
    form_field(body, "ip_gw",   ip_gw,   sizeof(ip_gw));
    form_field(body, "ip_dns",  ip_dns,  sizeof(ip_dns));

    bool   want_static = (strcmp(ip_mode, "static") == 0);
    ip4_addr_t addr;
    if (want_static && (!ip4addr_aton(ip_addr, &addr) || !ip4addr_aton(ip_mask, &addr))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid static IP configuration");
        return ESP_FAIL;
    }
    if (ip_gw[0]  && !ip4addr_aton(ip_gw, &addr)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid gateway");
        return ESP_FAIL;
    }
    if (ip_dns[0] && !ip4addr_aton(ip_dns, &addr)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid DNS");
        return ESP_FAIL;
    }

    s_ip_mode = want_static ? 1 : 0;
    strlcpy(s_static_ip,   ip_addr, sizeof(s_static_ip));
    strlcpy(s_static_mask, ip_mask, sizeof(s_static_mask));
    strlcpy(s_static_gw,   ip_gw,   sizeof(s_static_gw));
    strlcpy(s_static_dns,  ip_dns,  sizeof(s_static_dns));
    nvs_save_settings();

    esp_err_t err = nvs_save_creds(ssid, pass);
    send_page_head(req, T("EleRelay \xe2\x80\x94 Network", "EleRelay \xe2\x80\x94 V&otilde;rk"));
    if (err == ESP_OK) {
        char safe[CRED_LEN * 6] = {0};
        html_escape(ssid, safe, sizeof(safe));
        char html[640];
        snprintf(html, sizeof(html),
            "<h1>&#x2714; %s</h1>"
            "<p>%s <b>%s</b>... %s</p>"
            "<p class='note'>%s</p>"
            "</body></html>",
            T("Saved!", "Salvestatud!"),
            T("Connecting to", "&Uuml;hendan v&otilde;rguga"), safe,
            T("Restarting.", "Taask&auml;ivitan."),
            T("Returns to AP setup mode if connection fails.",
              "L&auml;heb AP-re&zcaron;iimi, kui &uuml;hendus nurjub."));
        httpd_resp_sendstr_chunk(req, html);
        httpd_resp_sendstr_chunk(req, NULL);
        xTaskCreate(restart_task, "rst", 2048, NULL, 3, NULL);
    } else {
        char errmsg[128];
        snprintf(errmsg, sizeof(errmsg),
            "<h1>&#x274C; %s</h1><p>%s</p></body></html>",
            T("Error", "Viga"),
            T("Failed to save credentials.", "Andmete salvestamine eba&otilde;nnestus."));
        httpd_resp_sendstr_chunk(req, errmsg);
        httpd_resp_sendstr_chunk(req, NULL);
    }
    return ESP_OK;
}

/* ── / (price table) ─────────────────────────────────────────────────────── */

static esp_err_t web_get_handler(httpd_req_t *req)
{
    char chunk[1024];

    send_page_head(req, T("Elering Smart Relay", "Elering Smart Relee"));
    {
        char h1[64];
        snprintf(h1, sizeof(h1), "<h1>&#x26A1; %s</h1>",
                 T("Elering Smart Relay", "Elering Smart Relee"));
        httpd_resp_sendstr_chunk(req, h1);
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool        relay_on       = s_relay_on;
    time_t      relay_on_since = s_relay_on_since;
    int         count          = s_hour_count;
    time_t      last_fetch     = s_last_fetch;
    bool        inv            = s_relay_inv;
    int         win_hours      = s_hours_window;
    int         min_run        = s_min_run_minutes;
    bool        aon_en         = s_always_on_en;
    int         aon_lim        = s_always_on_limit;
    bool        aoff_en        = s_always_off_en;
    int         aoff_lim       = s_always_off_limit;
    hour_slot_t local[MAX_SLOTS];
    memcpy(local, s_hours, count * sizeof(hour_slot_t));
    xSemaphoreGive(s_mutex);

    time_t now; struct tm ti;
    time(&now); localtime_r(&now, &ti);
    char time_str[32], fetch_str[32];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S %Z", &ti);

    /* Find first non-expired slot — simulate from there with actual relay state.
     * Starting simulation from slot 0 with current state corrupts relay_on_since
     * for past slots and produces wrong forced-ON predictions. */
    bool eff_on[MAX_SLOTS];
    {
        int sim_start = 0;
        while (sim_start < count && local[sim_start].ts + 900 <= now) sim_start++;
        for (int i = 0; i < sim_start; i++)
            eff_on[i] = local[i].cheap ^ inv;   /* past slots: price-only, not shown */
        if (sim_start < count)
            compute_effective_relay_states(local + sim_start, count - sim_start,
                                           inv, min_run,
                                           aon_en, aon_lim, aoff_en, aoff_lim,
                                           relay_on, relay_on_since,
                                           eff_on + sim_start);
    }
    if (last_fetch) {
        struct tm tf; localtime_r(&last_fetch, &tf);
        strftime(fetch_str, sizeof(fetch_str), "%H:%M:%S", &tf);
    } else {
        strlcpy(fetch_str, "never", sizeof(fetch_str));
    }

    char safe_ssid[CRED_LEN * 6] = {0};
    html_escape(s_wifi_ssid, safe_ssid, sizeof(safe_ssid));

    snprintf(chunk, sizeof(chunk),
        "<p>%s <b>%s</b></p>"
        "<p>%s <span class='status %s'>%s</span>%s</p>"
        "<p class='meta'>IP: %s &nbsp;|&nbsp; WiFi: %s"
        " &nbsp;|&nbsp; %s %s"
        " &nbsp;|&nbsp; %s %dh, %s %dh, %s %02d:00</p>",
        T("Time:", "Aeg:"), time_str,
        T("Relay:", "Relee:"), relay_on ? "on" : "off", relay_on ? "ON" : "OFF",
        inv ? T(" <span style='font-size:.8rem;color:#888'>(inverted)</span>",
                " <span style='font-size:.8rem;color:#888'>(p&ouml;&ouml;ratud)</span>") : "",
        s_ip, safe_ssid,
        T("Last fetch:", "Viimane p&auml;ring:"), fetch_str,
        T("Window:", "Aken:"), s_hours_window,
        T("cheap:", "odav:"), s_cheap_hours,
        T("fetch:", "p&auml;ring:"), s_fetch_hour);
    httpd_resp_sendstr_chunk(req, chunk);
    {
        char fbtn[256];
        snprintf(fbtn, sizeof(fbtn),
            "<form method='POST' action='/fetch' style='margin:10px 0'>"
            "<button type='submit' style='width:auto;padding:7px 18px;"
            "font-size:.9rem'>&#x21BB; %s</button></form>",
            T("Update prices now", "Uuenda hinnad"));
        httpd_resp_sendstr_chunk(req, fbtn);
    }

    if (count == 0) {
        char nomsg[128];
        snprintf(nomsg, sizeof(nomsg), "<p><i>%s</i></p>",
            T("No price data yet &mdash; fetching&hellip;",
              "Hinna andmed puuduvad &mdash; laadin&hellip;"));
        httpd_resp_sendstr_chunk(req, nomsg);
    } else {
        /* Skip 15-min slots that have already ended */
        int start = 0;
        while (start < count && local[start].ts + 900 <= now) start++;
        int avail = count - start;
        int show  = avail;

        snprintf(chunk, sizeof(chunk),
            "<p class='meta'>%s %d &times; 15min.</p>"
            "<table><tr><th>%s</th><th>%s</th><th>%s</th></tr>",
            T("Showing", "N&auml;itan"), show,
            T("Time (local)", "Aeg (kohalik)"),
            T("Price (c/kWh)", "Hind (s/kWh)"),
            T("Relay", "Relee"));
        httpd_resp_sendstr_chunk(req, chunk);

        time_t cur_hour = (now / 3600) * 3600;
        time_t win_base = local[0].ts;
        int    win_secs = (win_hours > 0 ? win_hours : 1) * 3600;
        for (int i = start; i < start + show; i++) {
            /* Insert window separator row at each hours-window boundary */
            int this_win = (int)((local[i].ts - win_base) / win_secs);
            int prev_win = (i > start) ? (int)((local[i-1].ts - win_base) / win_secs) : this_win - 1;
            if (this_win != prev_win) {
                time_t ws_ts = win_base + (time_t)this_win * win_secs;
                struct tm tws; localtime_r(&ws_ts, &tws);
                char ws_str[20]; strftime(ws_str, sizeof(ws_str), "%Y-%m-%d %H:%M", &tws);
                snprintf(chunk, sizeof(chunk),
                    "<tr class='win-sep'><td colspan='3'>"
                    "&#x1F5D3; %s %d &mdash; %s</td></tr>",
                    T("Window", "Aken"), this_win + 1, ws_str);
                httpd_resp_sendstr_chunk(req, chunk);
            }

            struct tm th; localtime_r(&local[i].ts, &th);
            char hr[20]; strftime(hr, sizeof(hr), "%Y-%m-%d %H:%M", &th);
            bool is_cur = (local[i].ts == cur_hour);
            bool ron    = eff_on[i];   /* effective relay state (min-run aware) */
            float cprice  = local[i].price / 10.0f;
            int this_h = (int)(local[i].ts / 3600);
            int prev_h = (i > start)            ? (int)(local[i-1].ts / 3600) : -1;
            int next_h = (i < start + show - 1) ? (int)(local[i+1].ts / 3600) : -1;
            char row_cls[64];
            snprintf(row_cls, sizeof(row_cls), "hg %s%s%s%s",
                ron ? "cheap" : "exp",
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
    send_page_head(req, "EleRelay \xe2\x80\x94 OTA Update");
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

/* ── /fetch POST ─────────────────────────────────────────────────────────── */

static esp_err_t fetch_post_handler(httpd_req_t *req)
{
    fetch_prices();
    update_relay();
    update_display();
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_sendstr(req, "");
    return ESP_OK;
}

/* ── Web server ──────────────────────────────────────────────────────────── */

static httpd_handle_t start_webserver(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.stack_size       = 8192;
    cfg.max_uri_handlers = 14;

    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server"); return NULL;
    }

    static const httpd_uri_t u_root_sta  = { "/",         HTTP_GET,  web_get_handler,      NULL };
    static const httpd_uri_t u_root_ap   = { "/",         HTTP_GET,  wifi_get_handler,     NULL };
    static const httpd_uri_t u_wifi_get  = { "/wifi",     HTTP_GET,  wifi_get_handler,     NULL };
    static const httpd_uri_t u_wifi_post = { "/wifi",     HTTP_POST, wifi_post_handler,    NULL };
    static const httpd_uri_t u_rel_get   = { "/relay",    HTTP_GET,  relay_get_handler,    NULL };
    static const httpd_uri_t u_rel_post  = { "/relay",    HTTP_POST, relay_post_handler,   NULL };
    static const httpd_uri_t u_set_get   = { "/settings", HTTP_GET,  settings_get_handler, NULL };
    static const httpd_uri_t u_set_post  = { "/settings", HTTP_POST, settings_post_handler,NULL };
    static const httpd_uri_t u_mqtt_get  = { "/mqtt",     HTTP_GET,  mqtt_get_handler,     NULL };
    static const httpd_uri_t u_mqtt_post = { "/mqtt",     HTTP_POST, mqtt_post_handler,    NULL };
    static const httpd_uri_t u_ota_get    = { "/ota",    HTTP_GET,  ota_get_handler,    NULL };
    static const httpd_uri_t u_ota_post   = { "/ota",    HTTP_POST, ota_post_handler,   NULL };
    static const httpd_uri_t u_fetch_post = { "/fetch",  HTTP_POST, fetch_post_handler, NULL };

    if (s_ap_mode) {
        httpd_register_uri_handler(srv, &u_root_ap);
    } else {
        httpd_register_uri_handler(srv, &u_root_sta);
        httpd_register_uri_handler(srv, &u_wifi_get);
        httpd_register_uri_handler(srv, &u_rel_get);
        httpd_register_uri_handler(srv, &u_rel_post);
        httpd_register_uri_handler(srv, &u_set_get);
        httpd_register_uri_handler(srv, &u_set_post);
        httpd_register_uri_handler(srv, &u_mqtt_get);
        httpd_register_uri_handler(srv, &u_mqtt_post);
        httpd_register_uri_handler(srv, &u_ota_get);
        httpd_register_uri_handler(srv, &u_ota_post);
        httpd_register_uri_handler(srv, &u_fetch_post);
    }
    httpd_register_uri_handler(srv, &u_wifi_post);

    ESP_LOGI(TAG, "HTTP server ready at http://%s/", s_ip);
    return srv;
}

/* ── Effective relay state simulation ───────────────────────────────────── */

/* Simulate the min-run logic across slots[0..count-1].
 * init_relay_on / init_relay_on_since: actual relay state just before slots[0].
 * out_relay_on[i] = true if relay will be ON during slots[i]. */
static void compute_effective_relay_states(
    const hour_slot_t *slots, int count,
    bool inv, int min_run_minutes,
    bool aon_en, int aon_lim,
    bool aoff_en, int aoff_lim,
    bool init_relay_on, time_t init_relay_on_since,
    bool *out_relay_on)
{
    bool   relay_on       = init_relay_on;
    time_t relay_on_since = init_relay_on_since;

    for (int i = 0; i < count; i++) {
        bool want_on = slots[i].cheap ^ inv;
        if (aon_en && slots[i].price <= (float)aon_lim) want_on = true;
        bool on = want_on;
        if (!on && min_run_minutes > 0 && relay_on && relay_on_since > 0) {
            if (slots[i].ts - relay_on_since < (time_t)min_run_minutes * 60)
                on = true;
        }
        if (aoff_en && slots[i].price >= (float)aoff_lim) on = false;
        if (on && !relay_on)
            relay_on_since = slots[i].ts;
        else if (!on)
            relay_on_since = 0;
        relay_on        = on;
        out_relay_on[i] = on;
    }
}

/* ── Display update ──────────────────────────────────────────────────────── */

static void update_display(void)
{
    if (s_screen == SCREEN_CONFIG) return;  /* don't overwrite config page */
    time_t now; time(&now);
    time_t cur_slot = (now / 900) * 900;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool   relay_on       = s_relay_on;
    time_t relay_on_since = s_relay_on_since;
    bool   inv            = s_relay_inv;
    int    min_run        = s_min_run_minutes;
    bool   aon_en         = s_always_on_en;
    int    aon_lim        = s_always_on_limit;
    bool   aoff_en        = s_always_off_en;
    int    aoff_lim       = s_always_off_limit;
    bool   ap_mode        = s_ap_mode;
    int    count          = s_hour_count;
    hour_slot_t local[MAX_SLOTS];
    memcpy(local, s_hours, count * sizeof(hour_slot_t));
    xSemaphoreGive(s_mutex);

    /* Find index of current slot */
    int cur_idx = 0;
    for (int i = 0; i < count; i++) {
        if (local[i].ts == cur_slot) { cur_idx = i; break; }
    }

    /* Compute effective relay state from cur_idx using actual relay state.
     * Past slots (before cur_idx) are not shown — fill with price-only. */
    bool eff_on[MAX_SLOTS];
    for (int i = 0; i < cur_idx; i++)
        eff_on[i] = local[i].cheap ^ inv;
    if (cur_idx < count)
        compute_effective_relay_states(local + cur_idx, count - cur_idx,
                                       inv, min_run,
                                       aon_en, aon_lim, aoff_en, aoff_lim,
                                       relay_on, relay_on_since,
                                       eff_on + cur_idx);

    /* Build display slots from current slot onward */
    disp_slot_t dslots[DISP_MAX_BARS];
    int dcount = 0;
    for (int i = cur_idx; i < count && dcount < DISP_MAX_BARS; i++, dcount++) {
        dslots[dcount].price_eur_mwh = local[i].price;
        dslots[dcount].is_cheap      = local[i].cheap;
        dslots[dcount].is_on         = eff_on[i];
    }

    int win_offset = (s_hours_window > 0) ? cur_idx % (s_hours_window * 4) : 0;
    display_update(relay_on, ap_mode, s_wifi_ssid, s_inet_ok,
                   now, dslots, dcount, 0,
                   s_cheap_hours, s_hours_window, win_offset);
}

/* ── Sysinfo helper for config page 3 ────────────────────────────────────── */

static void build_sysinfo(disp_sysinfo_t *out)
{
    const esp_app_desc_t *d = esp_app_get_description();
    out->app_ver    = d->version;
    out->build_date = d->date;
    out->build_time = d->time;
    out->ssid = s_ap_mode ? NULL : s_wifi_ssid;
    out->ip   = s_ip;

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out->mac, sizeof(out->mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(out->hostname, sizeof(out->hostname), "EleRelay-%02X%02X%02X",
             mac[3], mac[4], mac[5]);

    out->rssi    = 0;
    out->channel = 0;
    if (!s_ap_mode) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            out->rssi    = ap.rssi;
            out->channel = ap.primary;
        }
    }
    out->free_heap  = esp_get_free_heap_size();
    out->cpu_mhz    = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    out->uptime_sec = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    out->last_fetch = s_last_fetch;

    static const char *rsn_str[] = {
        "No data", "Schedule", "Always ON", "Min-run", "Always OFF"
    };
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    relay_rsn_t reason = s_relay_reason;
    xSemaphoreGive(s_mutex);
    strlcpy(out->relay_reason, rsn_str[reason], sizeof(out->relay_reason));
}

/* ── Touch task ──────────────────────────────────────────────────────────── */

static void touch_task(void *arg)
{
    bool      was_touched    = false;
    TickType_t cfg_entered_at = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));
        int tx, ty;
        bool touched = display_touch_read(&tx, &ty);

        /* Auto-close config screen after 60 s of inactivity */
        if (s_screen == SCREEN_CONFIG &&
            (xTaskGetTickCount() - cfg_entered_at) > pdMS_TO_TICKS(60000)) {
            s_screen = SCREEN_MAIN;
            update_display();
            was_touched = false;
            continue;
        }

        if (!touched) { was_touched = false; continue; }
        if (was_touched) continue;  /* wait for finger lift before next event */
        was_touched = true;

        if (s_screen == SCREEN_MAIN) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_cfg_window   = s_hours_window;
            s_cfg_cheap    = s_cheap_hours;
            s_cfg_min_run  = s_min_run_minutes;
            s_cfg_aon_en   = s_always_on_en;
            s_cfg_aoff_en  = s_always_off_en;
            s_cfg_aon_lim  = s_always_on_limit;
            s_cfg_aoff_lim = s_always_off_limit;
            s_cfg_bl_pct   = s_backlight_pct;
            xSemaphoreGive(s_mutex);
            s_cfg_page = 0;
            s_screen = SCREEN_CONFIG;
            cfg_entered_at = xTaskGetTickCount();
            {
                disp_sysinfo_t _si;
                build_sysinfo(&_si);
                display_show_config(s_cfg_cheap, s_cfg_window, s_cfg_min_run,
                                    s_cfg_aon_en, s_cfg_aon_lim,
                                    s_cfg_aoff_en, s_cfg_aoff_lim, s_cfg_page,
                                    s_cfg_bl_pct, &_si);
            }
        } else {
            cfg_entered_at = xTaskGetTickCount();  /* any touch resets the timer */
            int btn = display_config_hittest(tx, ty, s_cfg_page);
#define REDRAW() do { \
    disp_sysinfo_t _ri; \
    build_sysinfo(&_ri); \
    display_show_config(s_cfg_cheap, s_cfg_window, s_cfg_min_run, \
                        s_cfg_aon_en, s_cfg_aon_lim, \
                        s_cfg_aoff_en, s_cfg_aoff_lim, s_cfg_page, \
                        s_cfg_bl_pct, &_ri); \
} while(0)
            switch (btn) {
            case DISP_CFG_CLOSE:
                s_screen = SCREEN_MAIN;
                update_display();
                break;
            case DISP_CFG_NEXT_PAGE:
                if (s_cfg_page < 3) s_cfg_page++;
                REDRAW();
                break;
            case DISP_CFG_PREV_PAGE:
                if (s_cfg_page > 0) s_cfg_page--;
                REDRAW();
                break;
            case DISP_CFG_WIN_DEC:
                if (s_cfg_window > 2) s_cfg_window--;
                if (s_cfg_cheap >= s_cfg_window) s_cfg_cheap = s_cfg_window - 1;
                REDRAW();
                break;
            case DISP_CFG_WIN_INC:
                if (s_cfg_window < 24) s_cfg_window++;
                REDRAW();
                break;
            case DISP_CFG_CHE_DEC:
                if (s_cfg_cheap > 1) s_cfg_cheap--;
                REDRAW();
                break;
            case DISP_CFG_CHE_INC:
                if (s_cfg_cheap < s_cfg_window - 1) s_cfg_cheap++;
                REDRAW();
                break;
            case DISP_CFG_MIN_DEC:
                if (s_cfg_min_run > 0) s_cfg_min_run -= 15;
                REDRAW();
                break;
            case DISP_CFG_MIN_INC:
                if (s_cfg_min_run < 480) s_cfg_min_run += 15;
                REDRAW();
                break;
            case DISP_CFG_AON_TOG:
                s_cfg_aon_en = !s_cfg_aon_en;
                REDRAW();
                break;
            case DISP_CFG_AOFF_TOG:
                s_cfg_aoff_en = !s_cfg_aoff_en;
                REDRAW();
                break;
            case DISP_CFG_AON_DEC:
                if (s_cfg_aon_lim >= 10) s_cfg_aon_lim -= 10;
                REDRAW();
                break;
            case DISP_CFG_AON_INC:
                if (s_cfg_aon_lim <= 1990) s_cfg_aon_lim += 10;
                REDRAW();
                break;
            case DISP_CFG_AOFF_DEC:
                if (s_cfg_aoff_lim >= 10) s_cfg_aoff_lim -= 10;
                REDRAW();
                break;
            case DISP_CFG_AOFF_INC:
                if (s_cfg_aoff_lim <= 1990) s_cfg_aoff_lim += 10;
                REDRAW();
                break;
            case DISP_CFG_BL_DEC:
                if (s_cfg_bl_pct >= 5) s_cfg_bl_pct -= 5;
                else s_cfg_bl_pct = 0;
                display_set_backlight(s_cfg_bl_pct);
                REDRAW();
                break;
            case DISP_CFG_BL_INC:
                if (s_cfg_bl_pct <= 95) s_cfg_bl_pct += 5;
                else s_cfg_bl_pct = 100;
                display_set_backlight(s_cfg_bl_pct);
                REDRAW();
                break;
            case DISP_CFG_SAVE:
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                s_hours_window    = s_cfg_window;
                s_cheap_hours     = s_cfg_cheap;
                s_min_run_minutes = s_cfg_min_run;
                s_always_on_en    = s_cfg_aon_en;
                s_always_off_en   = s_cfg_aoff_en;
                s_always_on_limit  = s_cfg_aon_lim;
                s_always_off_limit = s_cfg_aoff_lim;
                s_backlight_pct   = s_cfg_bl_pct;
                mark_cheap_hours(s_hours, s_hour_count, s_min_run_minutes);
                xSemaphoreGive(s_mutex);
                display_set_backlight(s_backlight_pct);
                nvs_save_settings();
                update_relay();
                s_screen = SCREEN_MAIN;
                update_display();
                break;
            default: break;
            }
#undef REDRAW
        }
    }
}

/* ── Offline blink task: 3Hz "!" icon while no internet ─────────────────── */

static void offline_blink_task(void *arg)
{
    bool visible = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(167));
        if (s_screen != SCREEN_MAIN) {
            visible = false;   /* config page covers icon area; nothing to clear */
            continue;
        }
        if (!s_ap_mode && !s_inet_ok) {
            visible = !visible;
            display_blink_offline(visible);
        } else if (visible) {
            visible = false;
            display_blink_offline(false);
        }
    }
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
        vTaskDelay(pdMS_TO_TICKS(1000));    /* tick every second for clock */
        time_t now; time(&now);

        update_display();   /* refresh clock every second */

        if (now % 60 != 0) continue;   /* minute-level work below */

        struct tm ti; localtime_r(&now, &ti);

        if (s_inet_check_min > 0 &&
            now - s_last_inet_check >= (time_t)s_inet_check_min * 60) {
            s_last_inet_check = now;
            bool ok = inet_check();
            if (ok != s_inet_ok) {
                s_inet_ok = ok;
                ESP_LOGW(TAG, "Internet connectivity: %s", ok ? "OK" : "LOST");
                update_display();
            }
        }

        bool stale     = (s_hour_count == 0 || now - s_last_fetch > 25 * 3600);
        bool at_time   = (ti.tm_hour == s_fetch_hour);
        bool not_fresh = (now - s_last_fetch > 1800);   /* >30 min since last fetch */
        /* Trigger refetch when fewer than 1 window of prices remains */
        bool low_data  = (s_last_slot_ts > 0 && s_last_slot_ts > now &&
                          s_last_slot_ts - now < (time_t)s_hours_window * 3600 &&
                          now - s_last_fetch > 1800);

        if (stale || low_data || (at_time && not_fresh)) {
            ESP_LOGI(TAG, "Refreshing prices (stale=%d low_data=%d at_time=%d)",
                     stale, low_data, at_time);
            if (fetch_prices() != ESP_OK)
                ESP_LOGW(TAG, "Fetch failed, keeping old data");
        }
        update_relay();
    }
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

void app_main(void)
{
    ESP_LOGI(TAG, "EleRelay booting (relay GPIO%d)", CONFIG_RELAY_GPIO);

    /* Display + touch */
    display_init();
    display_touch_init();
    display_status("Booting...", NULL);

    /* NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase()); ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Load runtime settings (before anything that uses them) */
    nvs_load_settings();
    display_set_lang(s_lang);
    display_set_backlight(s_backlight_pct);
    ESP_LOGI(TAG, "Settings: window=%d cheap=%d inv=%d fetch_h=%d "
             "ntp=%s tz=%s mqtt=%d host=%s port=%d",
             s_hours_window, s_cheap_hours, s_relay_inv, s_fetch_hour,
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
    bool had_nvs_creds = (nvs_load_creds(ssid, pass) == ESP_OK);
    if (!had_nvs_creds) {
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
        display_status("Testing internet connection", s_ip);
        ntp_sync();
        mqtt_start();
        start_webserver();
        xTaskCreate(controller_task, "ctrl", 8192, NULL, 5, NULL);
        xTaskCreate(touch_task, "touch", 8192, NULL, 4, NULL);
        xTaskCreate(offline_blink_task, "blink", 2048, NULL, 2, NULL);
    } else {
        s_ap_mode = true;
        ESP_LOGW(TAG, "STA failed — AP mode");
        wifi_start_ap();
        start_webserver();
        update_display();   /* show AP mode on screen */
        xTaskCreate(touch_task, "touch", 8192, NULL, 4, NULL);
        if (had_nvs_creds)
            xTaskCreate(ap_reconnect_task, "ap_reconn", 2048, NULL, 3, NULL);
    }
}
