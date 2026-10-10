#pragma once
/*
 * LCD main screen, touch config screen and the offline "!" blink.
 */

void ui_init(void);

/* Redraw the main screen (no-op while the touch config screen is open) */
void update_display(void);

void ui_touch_task(void *arg);
void ui_offline_blink_task(void *arg);
