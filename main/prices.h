#pragma once
/*
 * Elering NPS price API and internet connectivity check. Both block for up
 * to several seconds — call them only from the network task (main.c).
 */
#include <stdbool.h>
#include "esp_err.h"
#include "scheduler.h"

/* Fetch 15-min price slots for the next MAX_SLOTS/4 hours, starting at the
 * current hour. Writes up to max slots to out, sorted by time, with
 * cheap=false. Returns ESP_OK and the number of slots in *count. */
esp_err_t prices_fetch(hour_slot_t *out, int max, int *count);

/* HEAD request to the Elering dashboard; true if it answered */
bool inet_check(void);
