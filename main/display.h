#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

typedef struct {
    float price_eur_mwh;
    bool  is_cheap;   /* price cheapness — drives CHEAP/EXPNS label */
    bool  is_on;      /* effective relay state (accounts for min_run forcing) */
} disp_slot_t;

#define DISP_MAX_BARS 96   /* 24h at 4 slots/h */

void display_init(void);
void display_status(const char *line1, const char *line2);
void display_update(bool relay_on, bool ap_mode, const char *ssid, bool inet_ok,
                    time_t now, const disp_slot_t *slots, int count, int cur_idx,
                    int cheap_hours, int hours_window, int win_offset);

/* Touch controller (XPT2046) */
void display_touch_init(void);
bool display_touch_read(int *x, int *y);   /* screen coords; true = finger down */

/* Config page */
#define DISP_CFG_CLOSE     0
#define DISP_CFG_WIN_DEC   1
#define DISP_CFG_WIN_INC   2
#define DISP_CFG_CHE_DEC   3
#define DISP_CFG_CHE_INC   4
#define DISP_CFG_SAVE      5
#define DISP_CFG_MIN_DEC   6
#define DISP_CFG_MIN_INC   7
#define DISP_CFG_AON_TOG   8   /* toggle always-on enable */
#define DISP_CFG_AOFF_TOG  9   /* toggle always-off enable */
#define DISP_CFG_NEXT_PAGE 10  /* page 1 → page 2 */
#define DISP_CFG_PREV_PAGE 11  /* page 2 → page 1 */
#define DISP_CFG_AON_DEC   12  /* always-on price limit − */
#define DISP_CFG_AON_INC   13  /* always-on price limit + */
#define DISP_CFG_AOFF_DEC  14  /* always-off price limit − */
#define DISP_CFG_AOFF_INC  15  /* always-off price limit + */
#define DISP_CFG_BL_DEC    16  /* backlight brightness − */
#define DISP_CFG_BL_INC    17  /* backlight brightness + */

typedef struct {
    const char *app_ver;
    const char *build_date;
    const char *build_time;
    const char *ssid;        /* NULL/empty in AP mode */
    const char *ip;
    char        mac[18];     /* "XX:XX:XX:XX:XX:XX" */
    char        hostname[24];
    int8_t      rssi;           /* dBm; 0 if not connected */
    uint8_t     channel;        /* 0 if not connected */
    uint32_t    free_heap;      /* bytes */
    uint32_t    cpu_mhz;
    uint32_t    uptime_sec;
    time_t      last_fetch;     /* 0 = never */
    char        relay_reason[16];
} disp_sysinfo_t;

void display_show_config(int cheap_hours, int hours_window, int min_run_minutes,
                          bool aon_en, int aon_lim_mwh,
                          bool aoff_en, int aoff_lim_mwh, int page,
                          int backlight_pct,
                          const disp_sysinfo_t *info);
int  display_config_hittest(int tx, int ty, int page);
void display_set_backlight(int pct);

/* Offline "!" icon: direct draw/clear at the reserved icon rect, for fast blink */
void display_blink_offline(bool visible);

/* Language */
#define LANG_EN 0
#define LANG_ET 1
void display_set_lang(int lang);
