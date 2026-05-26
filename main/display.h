#pragma once
#include <stdbool.h>
#include <time.h>

typedef struct {
    float price_eur_mwh;
    bool  is_cheap;   /* price cheapness — drives CHEAP/EXPNS label */
    bool  is_on;      /* effective relay state (accounts for min_run forcing) */
} disp_slot_t;

#define DISP_MAX_BARS 80

void display_init(void);
void display_status(const char *line1, const char *line2);
void display_update(bool relay_on, bool ap_mode, const char *ssid,
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

void display_show_config(int cheap_hours, int hours_window, int min_run_minutes,
                          bool aon_en, int aon_lim_mwh,
                          bool aoff_en, int aoff_lim_mwh, int page,
                          const char *app_ver, const char *build_date, const char *build_time);
int  display_config_hittest(int tx, int ty, int page);

/* Language */
#define LANG_EN 0
#define LANG_ET 1
void display_set_lang(int lang);
