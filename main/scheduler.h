#pragma once
/*
 * Price schedule logic: cheap-slot selection and the relay decision.
 *
 * Pure C with no ESP-IDF dependencies, so it can be unit-tested on a PC
 * (see test/host). Prices are EUR/MWh; slots are 15 minutes (900 s).
 */
#include <stdbool.h>
#include <time.h>

#define SLOT_SECS  900
#define MAX_SLOTS  112   /* 24 h × 4 slots/h + spare */

typedef struct {
    time_t ts;      /* slot start (UTC epoch) */
    float  price;   /* EUR/MWh */
    bool   cheap;   /* selected by mark_cheap_hours() */
} hour_slot_t;

typedef enum {
    RELAY_RSN_NO_DATA = 0,
    RELAY_RSN_SCHEDULE,
    RELAY_RSN_ALWAYS_ON,
    RELAY_RSN_MIN_RUN,
    RELAY_RSN_ALWAYS_OFF,
} relay_rsn_t;

/* Settings that decide the relay state for a given slot */
typedef struct {
    bool inv;               /* ON during expensive instead of cheap slots */
    int  min_run_minutes;   /* keep ON at least this long; 0 = off */
    bool aon_en;            /* always ON when price <= aon_lim */
    int  aon_lim;           /* EUR/MWh */
    bool aoff_en;           /* always OFF when price >= aoff_lim (wins) */
    int  aoff_lim;          /* EUR/MWh */
} relay_rules_t;

/* Relay state carried from one decision to the next */
typedef struct {
    bool   on;
    time_t on_since;        /* when the relay last turned ON; 0 when OFF */
} relay_state_t;

/* Mark the cheapest slots, independently per hours-window (windows are
 * anchored at slots[0].ts). See scheduler.c for the min-run block mode and
 * partial-window scaling. slots must be sorted by ts. */
void mark_cheap_hours(hour_slot_t *slots, int count,
                      int cheap_hours, int hours_window, int min_run_minutes);

/* Decide the relay state at time t. slot is the price slot covering t, or
 * NULL when there is no price data (relay OFF). Updates *st (including
 * on_since) and returns the reason for the decision. */
relay_rsn_t relay_decide(const hour_slot_t *slot, time_t t,
                         const relay_rules_t *rules, relay_state_t *st);

/* Forecast the relay state for slots[0..count-1], starting from the actual
 * relay state just before slots[0]. out_on[i] = relay ON during slots[i]. */
void relay_simulate(const hour_slot_t *slots, int count,
                    const relay_rules_t *rules, relay_state_t start,
                    bool *out_on);

/* Short English name of a relay reason ("Schedule", "Min-run", ...) */
const char *relay_reason_str(relay_rsn_t r);
