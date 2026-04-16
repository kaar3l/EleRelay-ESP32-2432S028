#pragma once
#include <stdbool.h>
#include <time.h>

typedef struct {
    float price_eur_mwh;
    bool  is_cheap;
} disp_slot_t;

#define DISP_MAX_BARS 24

void display_init(void);
void display_status(const char *line1, const char *line2);
void display_update(bool relay_on, bool ap_mode, const char *ssid,
                    time_t now, const disp_slot_t *slots, int count, int cur_idx,
                    int cheap_hours, int hours_window);
