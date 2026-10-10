/*
 * Runtime settings and WiFi credentials in NVS — see settings.h.
 */
#include <string.h>
#include "nvs.h"
#include "esp_log.h"
#include "esp_random.h"
#include "bootloader_random.h"
#include "display.h"     /* LANG_EN */
#include "settings.h"

static const char *TAG = "settings";

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
#define NVS_KEY_WINDOW    "h_window"
#define NVS_KEY_CHEAP     "cheap_h_n"
#define NVS_KEY_INV       "relay_inv"
#define NVS_KEY_FETCHH    "fetch_hour"
#define NVS_KEY_NTP_SRV   "ntp_server"
#define NVS_KEY_TZ        "tz_str"
#define NVS_KEY_MQTT_EN   "mqtt_en"
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
#define NVS_KEY_DEV_PASS  "dev_pass"

app_settings_t g_cfg = {
    .hours_window     = CONFIG_HOURS_WINDOW,
    .cheap_hours      = CONFIG_CHEAP_HOURS,
    .relay_inv        = false,
    .fetch_hour       = 23,
    .min_run_minutes  = CONFIG_MIN_RUN_MINUTES,
    .always_on_en     = false,
    .always_on_limit  = 0,
    .always_off_en    = false,
    .always_off_limit = 200,

    .lang             = LANG_EN,
    .backlight_pct    = 100,
    .inet_check_min   = 5,
    .ntp_server       = "pool.ntp.org",
    .tz_str           = "EET-2EEST,M3.5.0/3,M10.5.0/4",
    .dev_pass         = "",

    .mqtt_enabled     = false,
    .mqtt_host        = "",
    .mqtt_port        = 1883,
    .mqtt_topic_price = "elerelay/price",
    .mqtt_topic_relay = "elerelay/relay",

    .ip_mode          = 0,
    .static_ip        = "",
    .static_mask      = "255.255.255.0",
    .static_gw        = "",
    .static_dns       = "",
};

/* ── WiFi credentials ────────────────────────────────────────────────────── */

esp_err_t creds_load(char *ssid, char *pass)
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

esp_err_t creds_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, NVS_KEY_SSID, ssid);
    if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_PASS, pass);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "Saving WiFi credentials failed: %s", esp_err_to_name(err));
    return err;
}

/* ── Runtime settings ────────────────────────────────────────────────────── */

void settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;   /* first boot */

    app_settings_t *c = &g_cfg;
    uint8_t v;
    if (nvs_get_u8(h, NVS_KEY_WINDOW,   &v) == ESP_OK) c->hours_window   = v;
    if (nvs_get_u8(h, NVS_KEY_CHEAP,    &v) == ESP_OK) c->cheap_hours    = v;
    if (nvs_get_u8(h, NVS_KEY_INV,      &v) == ESP_OK) c->relay_inv      = (bool)v;
    if (nvs_get_u8(h, NVS_KEY_FETCHH,   &v) == ESP_OK) c->fetch_hour     = v;
    if (nvs_get_u8(h, NVS_KEY_MQTT_EN,  &v) == ESP_OK) c->mqtt_enabled   = (bool)v;
    if (nvs_get_u8(h, NVS_KEY_LANG,     &v) == ESP_OK) c->lang           = v;
    if (nvs_get_u8(h, NVS_KEY_BL,       &v) == ESP_OK) c->backlight_pct  = v;
    if (nvs_get_u8(h, NVS_KEY_IP_MODE,  &v) == ESP_OK) c->ip_mode        = v;
    if (nvs_get_u8(h, NVS_KEY_INET_CHK, &v) == ESP_OK) c->inet_check_min = v;
    if (nvs_get_u8(h, NVS_KEY_AON_EN,   &v) == ESP_OK) c->always_on_en   = (bool)v;
    if (nvs_get_u8(h, NVS_KEY_AOFF_EN,  &v) == ESP_OK) c->always_off_en  = (bool)v;

    uint16_t u16;
    if (nvs_get_u16(h, NVS_KEY_MQTT_PORT, &u16) == ESP_OK) c->mqtt_port       = u16;
    if (nvs_get_u16(h, NVS_KEY_MIN_RUN,   &u16) == ESP_OK) c->min_run_minutes = u16;

    int16_t lim;
    if (nvs_get_i16(h, NVS_KEY_AON_LIM,  &lim) == ESP_OK) c->always_on_limit  = lim;
    if (nvs_get_i16(h, NVS_KEY_AOFF_LIM, &lim) == ESP_OK) c->always_off_limit = lim;

    /* Strings: a missing key leaves the default in place */
#define GET_STR(key, field) do { size_t len = sizeof(c->field); \
                                 nvs_get_str(h, key, c->field, &len); } while (0)
    GET_STR(NVS_KEY_NTP_SRV,   ntp_server);
    GET_STR(NVS_KEY_TZ,        tz_str);
    GET_STR(NVS_KEY_MQTT_HOST, mqtt_host);
    GET_STR(NVS_KEY_MQTT_TPRC, mqtt_topic_price);
    GET_STR(NVS_KEY_MQTT_TRLY, mqtt_topic_relay);
    GET_STR(NVS_KEY_IP_ADDR,   static_ip);
    GET_STR(NVS_KEY_IP_MASK,   static_mask);
    GET_STR(NVS_KEY_IP_GW,     static_gw);
    GET_STR(NVS_KEY_IP_DNS,    static_dns);
    GET_STR(NVS_KEY_DEV_PASS,  dev_pass);
#undef GET_STR

    nvs_close(h);
}

/* Stops at the first failing write and logs which key failed, so a full
 * NVS partition or a bad value is visible instead of silently ignored.
 * Nothing is committed unless every write succeeded. */
esp_err_t settings_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    const app_settings_t *c = &g_cfg;
    const char *key = NULL;
#define TRY(k, call) do { if (err == ESP_OK) { key = (k); err = (call); } } while (0)
    TRY(NVS_KEY_WINDOW,    nvs_set_u8 (h, NVS_KEY_WINDOW,    (uint8_t)c->hours_window));
    TRY(NVS_KEY_CHEAP,     nvs_set_u8 (h, NVS_KEY_CHEAP,     (uint8_t)c->cheap_hours));
    TRY(NVS_KEY_INV,       nvs_set_u8 (h, NVS_KEY_INV,       (uint8_t)c->relay_inv));
    TRY(NVS_KEY_FETCHH,    nvs_set_u8 (h, NVS_KEY_FETCHH,    (uint8_t)c->fetch_hour));
    TRY(NVS_KEY_MQTT_EN,   nvs_set_u8 (h, NVS_KEY_MQTT_EN,   (uint8_t)c->mqtt_enabled));
    TRY(NVS_KEY_LANG,      nvs_set_u8 (h, NVS_KEY_LANG,      (uint8_t)c->lang));
    TRY(NVS_KEY_BL,        nvs_set_u8 (h, NVS_KEY_BL,        (uint8_t)c->backlight_pct));
    TRY(NVS_KEY_IP_MODE,   nvs_set_u8 (h, NVS_KEY_IP_MODE,   (uint8_t)c->ip_mode));
    TRY(NVS_KEY_INET_CHK,  nvs_set_u8 (h, NVS_KEY_INET_CHK,  (uint8_t)c->inet_check_min));
    TRY(NVS_KEY_AON_EN,    nvs_set_u8 (h, NVS_KEY_AON_EN,    (uint8_t)c->always_on_en));
    TRY(NVS_KEY_AOFF_EN,   nvs_set_u8 (h, NVS_KEY_AOFF_EN,   (uint8_t)c->always_off_en));
    TRY(NVS_KEY_MQTT_PORT, nvs_set_u16(h, NVS_KEY_MQTT_PORT, (uint16_t)c->mqtt_port));
    TRY(NVS_KEY_MIN_RUN,   nvs_set_u16(h, NVS_KEY_MIN_RUN,   (uint16_t)c->min_run_minutes));
    TRY(NVS_KEY_AON_LIM,   nvs_set_i16(h, NVS_KEY_AON_LIM,   (int16_t)c->always_on_limit));
    TRY(NVS_KEY_AOFF_LIM,  nvs_set_i16(h, NVS_KEY_AOFF_LIM,  (int16_t)c->always_off_limit));
    TRY(NVS_KEY_NTP_SRV,   nvs_set_str(h, NVS_KEY_NTP_SRV,   c->ntp_server));
    TRY(NVS_KEY_TZ,        nvs_set_str(h, NVS_KEY_TZ,        c->tz_str));
    TRY(NVS_KEY_MQTT_HOST, nvs_set_str(h, NVS_KEY_MQTT_HOST, c->mqtt_host));
    TRY(NVS_KEY_MQTT_TPRC, nvs_set_str(h, NVS_KEY_MQTT_TPRC, c->mqtt_topic_price));
    TRY(NVS_KEY_MQTT_TRLY, nvs_set_str(h, NVS_KEY_MQTT_TRLY, c->mqtt_topic_relay));
    TRY(NVS_KEY_IP_ADDR,   nvs_set_str(h, NVS_KEY_IP_ADDR,   c->static_ip));
    TRY(NVS_KEY_IP_MASK,   nvs_set_str(h, NVS_KEY_IP_MASK,   c->static_mask));
    TRY(NVS_KEY_IP_GW,     nvs_set_str(h, NVS_KEY_IP_GW,     c->static_gw));
    TRY(NVS_KEY_IP_DNS,    nvs_set_str(h, NVS_KEY_IP_DNS,    c->static_dns));
    TRY("(commit)",        nvs_commit(h));
#undef TRY
    nvs_close(h);

    if (err != ESP_OK)
        ESP_LOGE(TAG, "Saving settings failed at %s: %s", key, esp_err_to_name(err));
    return err;
}

/* ── Device password ─────────────────────────────────────────────────────── */

esp_err_t dev_pass_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, NVS_KEY_DEV_PASS, g_cfg.dev_pass);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "Saving device password failed: %s", esp_err_to_name(err));
    return err;
}

/* The bootloader entropy source is used because the RF-based RNG is not
 * active before WiFi starts. Ambiguous characters (0/O, 1/l/I) are left out
 * so the password is easy to copy from the LCD. */
void dev_pass_init(void)
{
    if (g_cfg.dev_pass[0]) return;
    static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
    uint8_t rnd[10];
    bootloader_random_enable();
    esp_fill_random(rnd, sizeof(rnd));
    bootloader_random_disable();
    for (int i = 0; i < 10; i++)
        g_cfg.dev_pass[i] = alphabet[rnd[i] % (sizeof(alphabet) - 1)];
    g_cfg.dev_pass[10] = '\0';
    dev_pass_save();
    ESP_LOGI(TAG, "Generated device password (see LCD info page)");
}
