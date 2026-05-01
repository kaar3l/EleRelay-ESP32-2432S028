#pragma once
#include <stdbool.h>
#include <time.h>

typedef struct {
    float price_eur_mwh;
    bool  is_cheap;
} disp_slot_t;

#define DISP_MAX_BARS 48

void display_init(void);
void display_status(const char *line1, const char *line2);
void display_update(bool relay_on, bool ap_mode, const char *ssid,
                    time_t now, const disp_slot_t *slots, int count, int cur_idx,
                    int cheap_hours, int hours_window);

/* Touch controller (XPT2046) */
void display_touch_init(void);
bool display_touch_read(int *x, int *y);   /* screen coords; true = finger down */

/* Config page */
#define DISP_CFG_CLOSE    0
#define DISP_CFG_WIN_DEC  1
#define DISP_CFG_WIN_INC  2
#define DISP_CFG_CHE_DEC  3
#define DISP_CFG_CHE_INC  4
#define DISP_CFG_SAVE     5

void display_show_config(int cheap_hours, int hours_window);
int  display_config_hittest(int tx, int ty);

/* Language */
#define LANG_EN 0
#define LANG_ET 1
void display_set_lang(int lang);
