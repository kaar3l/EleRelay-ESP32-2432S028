/*
 * LCD main screen, touch config screen and offline blink — see ui.h.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "display.h"
#include "settings.h"
#include "wifi.h"
#include "app.h"
#include "ui.h"

/* ── Screen state ────────────────────────────────────────────────────────── */

typedef enum { SCREEN_MAIN, SCREEN_CONFIG } screen_t;
static volatile screen_t s_screen = SCREEN_MAIN;

/* Guards s_screen together with whatever is drawn for it, so the main screen
 * and the touch config screen can never overwrite each other. */
static SemaphoreHandle_t s_screen_mutex;

/* Values being edited on the touch config screen (applied on SAVE) */
static int  s_cfg_window   = 0;
static int  s_cfg_cheap    = 0;
static int  s_cfg_min_run  = 0;
static bool s_cfg_aon_en   = false;
static bool s_cfg_aoff_en  = false;
static int  s_cfg_aon_lim  = 0;
static int  s_cfg_aoff_lim = 200;
static int  s_cfg_bl_pct   = 100;
static int  s_cfg_page     = 0;

void ui_init(void)
{
    s_screen_mutex = xSemaphoreCreateMutex();
}

static void set_screen(screen_t scr)
{
    xSemaphoreTake(s_screen_mutex, portMAX_DELAY);
    s_screen = scr;
    xSemaphoreGive(s_screen_mutex);
}

/* ── Main screen ─────────────────────────────────────────────────────────── */

void update_display(void)
{
    if (s_screen == SCREEN_CONFIG) return;  /* cheap early-out; re-checked below */
    time_t now; time(&now);
    time_t cur_slot = (now / SLOT_SECS) * SLOT_SECS;

    static app_forecast_t fc;   /* ~2 KB; static, only used under s_screen_mutex */
    xSemaphoreTake(s_screen_mutex, portMAX_DELAY);
    if (s_screen != SCREEN_MAIN) {
        xSemaphoreGive(s_screen_mutex);
        return;
    }
    app_forecast(&fc, now);

    /* If the first unfinished slot covers "now" it is the current slot;
     * otherwise there is no price for now (stale data or a gap) and nothing
     * is shown as current. */
    int disp_cur = (fc.first < fc.count && fc.slots[fc.first].ts == cur_slot) ? 0 : -1;

    /* Display slots from the current slot onward */
    disp_slot_t dslots[DISP_MAX_BARS];
    int dcount = 0;
    for (int i = fc.first; i < fc.count && dcount < DISP_MAX_BARS; i++, dcount++) {
        dslots[dcount].price_eur_mwh = fc.slots[i].price;
        dslots[dcount].is_cheap      = fc.slots[i].cheap;
        dslots[dcount].is_on         = fc.on[i];
    }

    /* Offset of the first shown bar inside its hours-window (windows are
     * anchored at slots[0].ts, see mark_cheap_hours) */
    int win_offset = 0;
    if (g_cfg.hours_window > 0 && fc.first < fc.count)
        win_offset = (int)((fc.slots[fc.first].ts - fc.slots[0].ts) / SLOT_SECS)
                     % (g_cfg.hours_window * 4);

    display_update(fc.relay_on, g_ap_mode, g_wifi_ssid, g_state.inet_ok,
                   now, dslots, dcount, disp_cur,
                   g_cfg.cheap_hours, g_cfg.hours_window, win_offset);
    xSemaphoreGive(s_screen_mutex);
}

/* ── Config page 4: system info ──────────────────────────────────────────── */

static void build_sysinfo(disp_sysinfo_t *out)
{
    const esp_app_desc_t *d = esp_app_get_description();
    out->app_ver    = d->version;
    out->build_date = d->date;
    out->build_time = d->time;
    out->ssid       = g_ap_mode ? NULL : g_wifi_ssid;
    out->web_pass   = g_cfg.dev_pass;
    out->ip         = g_ip;

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out->mac, sizeof(out->mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(out->hostname, sizeof(out->hostname), "EleRelay-%02X%02X%02X",
             mac[3], mac[4], mac[5]);

    out->rssi    = 0;
    out->channel = 0;
    if (!g_ap_mode) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            out->rssi    = ap.rssi;
            out->channel = ap.primary;
        }
    }
    out->free_heap  = esp_get_free_heap_size();
    out->cpu_mhz    = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    out->uptime_sec = (uint32_t)(esp_timer_get_time() / 1000000ULL);

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    out->last_fetch    = g_state.last_fetch;
    relay_rsn_t reason = g_state.relay_reason;
    xSemaphoreGive(g_state_mutex);
    strlcpy(out->relay_reason, relay_reason_str(reason), sizeof(out->relay_reason));
}

static void draw_config(void)
{
    disp_sysinfo_t si;
    build_sysinfo(&si);
    xSemaphoreTake(s_screen_mutex, portMAX_DELAY);
    if (s_screen == SCREEN_CONFIG)
        display_show_config(s_cfg_cheap, s_cfg_window, s_cfg_min_run,
                            s_cfg_aon_en, s_cfg_aon_lim,
                            s_cfg_aoff_en, s_cfg_aoff_lim, s_cfg_page,
                            s_cfg_bl_pct, &si);
    xSemaphoreGive(s_screen_mutex);
}

/* ── Touch task ──────────────────────────────────────────────────────────── */

static void open_config(void)
{
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    s_cfg_window   = g_cfg.hours_window;
    s_cfg_cheap    = g_cfg.cheap_hours;
    s_cfg_min_run  = g_cfg.min_run_minutes;
    s_cfg_aon_en   = g_cfg.always_on_en;
    s_cfg_aoff_en  = g_cfg.always_off_en;
    s_cfg_aon_lim  = g_cfg.always_on_limit;
    s_cfg_aoff_lim = g_cfg.always_off_limit;
    s_cfg_bl_pct   = g_cfg.backlight_pct;
    xSemaphoreGive(g_state_mutex);
    s_cfg_page = 0;
    set_screen(SCREEN_CONFIG);
    draw_config();
}

static void save_config(void)
{
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    g_cfg.hours_window     = s_cfg_window;
    g_cfg.cheap_hours      = s_cfg_cheap;
    g_cfg.min_run_minutes  = s_cfg_min_run;
    g_cfg.always_on_en     = s_cfg_aon_en;
    g_cfg.always_off_en    = s_cfg_aoff_en;
    g_cfg.always_on_limit  = s_cfg_aon_lim;
    g_cfg.always_off_limit = s_cfg_aoff_lim;
    g_cfg.backlight_pct    = s_cfg_bl_pct;
    app_recompute_cheap();
    xSemaphoreGive(g_state_mutex);
    display_set_backlight(g_cfg.backlight_pct);
    /* On failure the settings still apply until reboot; settings_save()
     * logs the error (there is no room on the LCD flow to show it). */
    settings_save();
    update_relay();
}

void ui_touch_task(void *arg)
{
    bool       was_touched    = false;
    TickType_t cfg_entered_at = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));
        int tx, ty;
        bool touched = display_touch_read(&tx, &ty);

        /* Auto-close config screen after 60 s of inactivity */
        if (s_screen == SCREEN_CONFIG &&
            (xTaskGetTickCount() - cfg_entered_at) > pdMS_TO_TICKS(60000)) {
            set_screen(SCREEN_MAIN);
            update_display();
            was_touched = false;
            continue;
        }

        if (!touched) { was_touched = false; continue; }
        if (was_touched) continue;  /* wait for finger lift before next event */
        was_touched = true;
        cfg_entered_at = xTaskGetTickCount();  /* any touch resets the timer */

        if (s_screen == SCREEN_MAIN) {
            open_config();
            continue;
        }

        switch (display_config_hittest(tx, ty, s_cfg_page)) {
        case DISP_CFG_CLOSE:
            set_screen(SCREEN_MAIN);
            update_display();
            continue;
        case DISP_CFG_SAVE:
            save_config();
            set_screen(SCREEN_MAIN);
            update_display();
            continue;
        case DISP_CFG_NEXT_PAGE:
            if (s_cfg_page < 3) s_cfg_page++;
            break;
        case DISP_CFG_PREV_PAGE:
            if (s_cfg_page > 0) s_cfg_page--;
            break;
        case DISP_CFG_WIN_DEC:
            if (s_cfg_window > 2) s_cfg_window--;
            if (s_cfg_cheap >= s_cfg_window) s_cfg_cheap = s_cfg_window - 1;
            break;
        case DISP_CFG_WIN_INC:
            if (s_cfg_window < 24) s_cfg_window++;
            break;
        case DISP_CFG_CHE_DEC:
            if (s_cfg_cheap > 1) s_cfg_cheap--;
            break;
        case DISP_CFG_CHE_INC:
            if (s_cfg_cheap < s_cfg_window - 1) s_cfg_cheap++;
            break;
        case DISP_CFG_MIN_DEC:
            if (s_cfg_min_run > 0) s_cfg_min_run -= 15;
            break;
        case DISP_CFG_MIN_INC:
            if (s_cfg_min_run < 480) s_cfg_min_run += 15;
            break;
        case DISP_CFG_AON_TOG:
            s_cfg_aon_en = !s_cfg_aon_en;
            break;
        case DISP_CFG_AOFF_TOG:
            s_cfg_aoff_en = !s_cfg_aoff_en;
            break;
        case DISP_CFG_AON_DEC:
            s_cfg_aon_lim = clamp(s_cfg_aon_lim - 10, PRICE_LIM_MIN, PRICE_LIM_MAX);
            break;
        case DISP_CFG_AON_INC:
            s_cfg_aon_lim = clamp(s_cfg_aon_lim + 10, PRICE_LIM_MIN, PRICE_LIM_MAX);
            break;
        case DISP_CFG_AOFF_DEC:
            s_cfg_aoff_lim = clamp(s_cfg_aoff_lim - 10, PRICE_LIM_MIN, PRICE_LIM_MAX);
            break;
        case DISP_CFG_AOFF_INC:
            s_cfg_aoff_lim = clamp(s_cfg_aoff_lim + 10, PRICE_LIM_MIN, PRICE_LIM_MAX);
            break;
        case DISP_CFG_BL_DEC:
            s_cfg_bl_pct = clamp(s_cfg_bl_pct - 5, 0, 100);
            display_set_backlight(s_cfg_bl_pct);
            break;
        case DISP_CFG_BL_INC:
            s_cfg_bl_pct = clamp(s_cfg_bl_pct + 5, 0, 100);
            display_set_backlight(s_cfg_bl_pct);
            break;
        default:
            continue;   /* tap outside any button: nothing to redraw */
        }
        draw_config();
    }
}

/* ── Offline blink task: 3 Hz "!" icon while there is no internet ────────── */

void ui_offline_blink_task(void *arg)
{
    bool visible = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(167));
        xSemaphoreTake(s_screen_mutex, portMAX_DELAY);
        if (s_screen != SCREEN_MAIN) {
            visible = false;   /* config page covers icon area; nothing to clear */
        } else if (!g_ap_mode && !g_state.inet_ok) {
            visible = !visible;
            display_blink_offline(visible);
        } else if (visible) {
            visible = false;
            display_blink_offline(false);
        }
        xSemaphoreGive(s_screen_mutex);
    }
}
