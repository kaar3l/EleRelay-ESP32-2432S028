/*
 * EleRelay — Elering spot-price relay for the ESP32-2432S028 ("Cheap Yellow
 * Display").
 *
 * Boot: load settings → try WiFi station → fall back to setup AP
 * "EleRelay-Setup". Tasks:
 *   controller  1 s tick: clock redraw; once a minute: relay update and
 *               scheduling of price fetches / internet checks
 *   net         performs the (slow) HTTPS requests so nothing else blocks
 *   touch/blink LCD interaction (ui.c), plus the HTTP server (web.c)
 *
 * Module overview: scheduler.c (pure price/relay logic), prices.c (Elering
 * API), settings.c (NVS), wifi.c, mqtt_pub.c, web.c, ui.c, display.c.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_ota_ops.h"
#include "driver/gpio.h"

#include "display.h"
#include "scheduler.h"
#include "settings.h"
#include "prices.h"
#include "wifi.h"
#include "mqtt_pub.h"
#include "web.h"
#include "ui.h"
#include "app.h"

static const char *TAG = "elerelay";

#define RELAY_GPIO       ((gpio_num_t)CONFIG_RELAY_GPIO)
#define RELAY_ON_LEVEL   (CONFIG_RELAY_ACTIVE_LOW ? 0 : 1)
#define RELAY_OFF_LEVEL  (CONFIG_RELAY_ACTIVE_LOW ? 1 : 0)

/* On-board RGB LED — active LOW */
#define LED_RED_GPIO    4
#define LED_GREEN_GPIO  16
#define LED_BLUE_GPIO   17
#define LED_ON  0
#define LED_OFF 1

app_state_t       g_state = { .inet_ok = true };
SemaphoreHandle_t g_state_mutex;

/* ── Shared helpers (app.h) ──────────────────────────────────────────────── */

relay_rules_t app_relay_rules(void)
{
    return (relay_rules_t){
        .inv             = g_cfg.relay_inv,
        .min_run_minutes = g_cfg.min_run_minutes,
        .aon_en          = g_cfg.always_on_en,
        .aon_lim         = g_cfg.always_on_limit,
        .aoff_en         = g_cfg.always_off_en,
        .aoff_lim        = g_cfg.always_off_limit,
    };
}

void app_recompute_cheap(void)
{
    mark_cheap_hours(g_state.slots, g_state.count,
                     g_cfg.cheap_hours, g_cfg.hours_window, g_cfg.min_run_minutes);
}

void app_forecast(app_forecast_t *f, time_t now)
{
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    f->count      = g_state.count;
    memcpy(f->slots, g_state.slots, f->count * sizeof(hour_slot_t));
    f->relay_on   = g_state.relay.on;
    f->last_fetch = g_state.last_fetch;
    relay_state_t start = g_state.relay;
    relay_rules_t rules = app_relay_rules();
    xSemaphoreGive(g_state_mutex);

    /* Forecast from the first slot that has not ended, starting from the
     * actual relay state. Simulating from slot 0 would corrupt on_since for
     * past slots and give wrong min-run predictions. */
    f->first = 0;
    while (f->first < f->count && f->slots[f->first].ts + SLOT_SECS <= now) f->first++;
    for (int i = 0; i < f->first; i++) f->on[i] = false;   /* past: not shown */
    relay_simulate(f->slots + f->first, f->count - f->first, &rules, start,
                   f->on + f->first);
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
}

void app_restart_later(void)
{
    xTaskCreate(restart_task, "rst", 2048, NULL, 3, NULL);
}

/* ── Relay ───────────────────────────────────────────────────────────────── */

void update_relay(void)
{
    time_t now; time(&now);
    time_t cur_slot = (now / SLOT_SECS) * SLOT_SECS;

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    hour_slot_t cur;
    bool found = false;
    for (int i = 0; i < g_state.count; i++) {
        if (g_state.slots[i].ts == cur_slot) { cur = g_state.slots[i]; found = true; break; }
    }
    relay_rules_t rules = app_relay_rules();
    g_state.relay_reason = relay_decide(found ? &cur : NULL, now, &rules, &g_state.relay);
    bool on = g_state.relay.on;
    xSemaphoreGive(g_state_mutex);

    bool is_cheap = found && cur.cheap;
    gpio_set_level(RELAY_GPIO, on ? RELAY_ON_LEVEL : RELAY_OFF_LEVEL);

    /* RGB LED: green = cheap, red = expensive, blue always off */
    gpio_set_level(LED_GREEN_GPIO, is_cheap ? LED_ON : LED_OFF);
    gpio_set_level(LED_RED_GPIO,   is_cheap ? LED_OFF : LED_ON);
    gpio_set_level(LED_BLUE_GPIO,  LED_OFF);

    ESP_LOGI(TAG, "Relay → %s (slot %s, inv=%d)",
             on ? "ON" : "OFF", is_cheap ? "cheap" : "expensive", g_cfg.relay_inv);

    /* MQTT publish relay state and current price */
    mqtt_publish(g_cfg.mqtt_topic_relay, on ? "1" : "0");
    if (found) {
        char pbuf[16];
        snprintf(pbuf, sizeof(pbuf), "%.3f", cur.price / 10.0f);   /* EUR/MWh → c/kWh */
        mqtt_publish(g_cfg.mqtt_topic_price, pbuf);
    }
}

/* ── Network task ────────────────────────────────────────────────────────────
 * All slow HTTPS requests run here, so neither the controller (clock,
 * relay) nor the web server waits for them. Others post requests as
 * event bits. */

static EventGroupHandle_t s_net_eg;
#define NET_REQ_FETCH  BIT0
#define NET_REQ_INET   BIT1

static volatile bool s_fetch_busy = false;   /* requested or running */

void app_request_fetch(void)
{
    if (s_fetch_busy) return;
    s_fetch_busy = true;
    xEventGroupSetBits(s_net_eg, NET_REQ_FETCH);
}

bool app_fetch_busy(void) { return s_fetch_busy; }

static void net_do_fetch(void)
{
    static hour_slot_t buf[MAX_SLOTS];   /* only used by this task */
    int n = 0;
    if (prices_fetch(buf, MAX_SLOTS, &n) != ESP_OK) {
        ESP_LOGW(TAG, "Price fetch failed, keeping old data");
        return;
    }
    time_t now; time(&now);

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    memcpy(g_state.slots, buf, n * sizeof(hour_slot_t));
    g_state.count        = n;
    g_state.last_fetch   = now;
    g_state.last_slot_ts = (n > 0) ? buf[n - 1].ts + SLOT_SECS : 0;
    app_recompute_cheap();
    xSemaphoreGive(g_state_mutex);

    update_relay();
    update_display();
}

static void net_task(void *arg)
{
    for (;;) {
        EventBits_t req = xEventGroupWaitBits(s_net_eg, NET_REQ_FETCH | NET_REQ_INET,
                                              pdTRUE, pdFALSE, portMAX_DELAY);
        if (req & NET_REQ_INET) {
            bool ok = inet_check();
            if (ok != g_state.inet_ok) {
                g_state.inet_ok = ok;
                ESP_LOGW(TAG, "Internet connectivity: %s", ok ? "OK" : "LOST");
                update_display();
            }
        }
        if (req & NET_REQ_FETCH) {
            net_do_fetch();
            s_fetch_busy = false;
        }
    }
}

/* ── Controller task ─────────────────────────────────────────────────────── */

static void controller_task(void *arg)
{
    app_request_fetch();   /* initial fetch */

    time_t last_minute = 0, last_inet_check = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));    /* tick every second for clock */
        time_t now; time(&now);

        update_display();   /* refresh clock every second */

        /* Minute-level work below. Compare minute numbers instead of testing
         * now % 60 == 0: loop iterations take a bit over 1 s, so an exact
         * second can be skipped. */
        if (now / 60 == last_minute) continue;
        last_minute = now / 60;

        if (g_cfg.inet_check_min > 0 &&
            now - last_inet_check >= (time_t)g_cfg.inet_check_min * 60) {
            last_inet_check = now;
            xEventGroupSetBits(s_net_eg, NET_REQ_INET);
        }

        xSemaphoreTake(g_state_mutex, portMAX_DELAY);
        int    count        = g_state.count;
        time_t last_fetch   = g_state.last_fetch;
        time_t last_slot_ts = g_state.last_slot_ts;
        xSemaphoreGive(g_state_mutex);

        struct tm ti; localtime_r(&now, &ti);
        bool stale     = (count == 0 || now - last_fetch > 25 * 3600);
        bool at_time   = (ti.tm_hour == g_cfg.fetch_hour);
        bool not_fresh = (now - last_fetch > 1800);   /* >30 min since last fetch */
        /* Refetch when fewer than one window of prices remains */
        bool low_data  = (last_slot_ts > now &&
                          last_slot_ts - now < (time_t)g_cfg.hours_window * 3600 &&
                          not_fresh);

        if ((stale || low_data || (at_time && not_fresh)) && !app_fetch_busy()) {
            ESP_LOGI(TAG, "Refreshing prices (stale=%d low_data=%d at_time=%d)",
                     stale, low_data, at_time);
            app_request_fetch();
        }
        update_relay();
    }
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

void app_main(void)
{
    /* Relay + RGB LED GPIOs — drive OFF before anything else. The output
     * latch is written before the pin becomes an output, so an active-LOW
     * relay never sees a LOW pulse during boot. */
    gpio_set_level(RELAY_GPIO,     RELAY_OFF_LEVEL);
    gpio_set_level(LED_RED_GPIO,   LED_OFF);
    gpio_set_level(LED_GREEN_GPIO, LED_OFF);
    gpio_set_level(LED_BLUE_GPIO,  LED_OFF);
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
    settings_load();
    dev_pass_init();
    display_set_ap_password(g_cfg.dev_pass);
    display_set_lang(g_cfg.lang);
    display_set_backlight(g_cfg.backlight_pct);
    ESP_LOGI(TAG, "Settings: window=%d cheap=%d inv=%d fetch_h=%d "
             "ntp=%s tz=%s mqtt=%d host=%s port=%d",
             g_cfg.hours_window, g_cfg.cheap_hours, g_cfg.relay_inv, g_cfg.fetch_hour,
             g_cfg.ntp_server, g_cfg.tz_str, g_cfg.mqtt_enabled, g_cfg.mqtt_host,
             g_cfg.mqtt_port);

    g_state_mutex = xSemaphoreCreateMutex();
    s_net_eg      = xEventGroupCreate();
    mqtt_pub_init();
    ui_init();
    wifi_init();

    /* WiFi credentials: NVS → Kconfig fallback */
    char ssid[CRED_LEN] = {0}, pass[CRED_LEN] = {0};
    bool had_nvs_creds = (creds_load(ssid, pass) == ESP_OK);
    if (!had_nvs_creds) {
        strlcpy(ssid, CONFIG_WIFI_SSID,     sizeof(ssid));
        strlcpy(pass, CONFIG_WIFI_PASSWORD, sizeof(pass));
        ESP_LOGI(TAG, "No NVS creds, using Kconfig defaults (SSID: %s)", ssid);
    } else {
        ESP_LOGI(TAG, "NVS creds loaded (SSID: %s)", ssid);
    }
    strlcpy(g_wifi_ssid, ssid, sizeof(g_wifi_ssid));

    char wifi_msg[72];
    snprintf(wifi_msg, sizeof(wifi_msg), "SSID: %s", ssid);
    display_status("Connecting to WiFi", wifi_msg);

    /* Boot reached this point without crashing — cancel any pending
     * rollback so the bootloader doesn't revert to the previous OTA slot. */
    esp_ota_mark_app_valid_cancel_rollback();

    if (wifi_start_sta(ssid, pass)) {
        g_ap_mode = false;
        display_status("Testing internet", g_ip);
        ntp_sync();
        mqtt_start();
        web_start();
        xTaskCreate(net_task,              "net",     8192, NULL, 4, NULL);
        xTaskCreate(controller_task,       "ctrl",    6144, NULL, 5, NULL);
        xTaskCreate(ui_touch_task,         "touch",   8192, NULL, 4, NULL);
        xTaskCreate(ui_offline_blink_task, "blink",   2048, NULL, 2, NULL);
        xTaskCreate(wifi_watchdog_task,    "wifi_wd", 2048, NULL, 3, NULL);
    } else {
        g_ap_mode = true;
        ESP_LOGW(TAG, "STA failed — AP mode");
        wifi_start_ap();
        web_start();
        update_display();   /* show AP mode + setup instructions on screen */
        xTaskCreate(ui_touch_task, "touch", 8192, NULL, 4, NULL);
        if (had_nvs_creds)
            xTaskCreate(ap_reconnect_task, "ap_reconn", 2048, NULL, 3, NULL);
    }
}
