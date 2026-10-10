/*
 * Price schedule logic — see scheduler.h. No ESP-IDF dependencies.
 */
#include <string.h>
#include "scheduler.h"

/* Mark the cheapest slots as cheap=true, independently per hours-window.
 *
 * Without min_run: picks the cheapest cheap_n individual 15-min slots.
 * With min_run:    picks contiguous blocks of blk=ceil(min_run/15) slots so the
 *                  relay never activates for less than min_run minutes.
 *                  Number of blocks = max(1, cheap_n / blk).
 *
 * A window shorter than hours_window (normally the last one, before the next
 * day's prices are published) gets a proportional share of cheap slots.
 * Otherwise cheap_n >= window length would mark the whole remainder cheap. */
void mark_cheap_hours(hour_slot_t *slots, int count,
                      int cheap_hours, int hours_window, int min_run_minutes)
{
    if (count <= 0) return;
    if (count > MAX_SLOTS) count = MAX_SLOTS;
    for (int i = 0; i < count; i++) slots[i].cheap = false;

    int    win_slots = (hours_window > 0 ? hours_window : 1) * 4;
    int    win_secs  = win_slots * SLOT_SECS;
    int    blk       = (min_run_minutes > 15) ? (min_run_minutes + 14) / 15 : 0;
    time_t base      = slots[0].ts;

    int i = 0;
    while (i < count) {
        /* Find all slots belonging to the same window */
        int wn = (int)((slots[i].ts - base) / win_secs);
        int j  = i;
        while (j < count && (int)((slots[j].ts - base) / win_secs) == wn) j++;
        int wlen = j - i;

        /* Cheap slots for this window, scaled for a partial window (rounded) */
        int cheap_n = cheap_hours * 4;
        if (wlen < win_slots)
            cheap_n = (cheap_n * wlen + win_slots / 2) / win_slots;
        if (cheap_n <= 0) { i = j; continue; }

        if (blk > 1 && blk <= wlen) {
            /* Block mode: greedily pick cheapest non-overlapping runs of blk slots */
            int n_blocks = cheap_n / blk;
            if (n_blocks < 1) n_blocks = 1;

            bool used[MAX_SLOTS];
            memset(used, 0, sizeof(bool) * wlen);

            for (int b = 0; b < n_blocks; b++) {
                float best_sum = 1e18f;
                int   best_s   = -1;
                for (int s = i; s + blk <= j; s++) {
                    bool overlap = false;
                    for (int k = 0; k < blk; k++)
                        if (used[s - i + k]) { overlap = true; break; }
                    if (overlap) continue;
                    float sum = 0;
                    for (int k = 0; k < blk; k++) sum += slots[s + k].price;
                    if (sum < best_sum) { best_sum = sum; best_s = s; }
                }
                if (best_s < 0) break;
                for (int k = 0; k < blk; k++) {
                    slots[best_s + k].cheap = true;
                    used[best_s - i + k]    = true;
                }
            }
        } else {
            /* Mark cheapest cheap_n individual slots (insertion sort) */
            int idx[MAX_SLOTS];
            for (int k = 0; k < wlen; k++) idx[k] = i + k;
            for (int a = 1; a < wlen; a++) {
                int key = idx[a], b = a - 1;
                while (b >= 0 && slots[idx[b]].price > slots[key].price)
                    { idx[b+1] = idx[b]; b--; }
                idx[b+1] = key;
            }
            int n = wlen < cheap_n ? wlen : cheap_n;
            for (int k = 0; k < n; k++) slots[idx[k]].cheap = true;
        }

        i = j;
    }
}

/* Priority, lowest to highest: schedule → always ON → min-run → always OFF */
relay_rsn_t relay_decide(const hour_slot_t *slot, time_t t,
                         const relay_rules_t *r, relay_state_t *st)
{
    bool on;
    relay_rsn_t reason;

    if (!slot) {
        on = false;
        reason = RELAY_RSN_NO_DATA;
    } else {
        on = slot->cheap ^ r->inv;
        reason = RELAY_RSN_SCHEDULE;

        if (r->aon_en && slot->price <= (float)r->aon_lim) {
            on = true;
            reason = RELAY_RSN_ALWAYS_ON;
        }

        if (!on && r->min_run_minutes > 0 && st->on && st->on_since > 0 &&
            t - st->on_since < (time_t)r->min_run_minutes * 60) {
            on = true;
            reason = RELAY_RSN_MIN_RUN;
        }

        if (r->aoff_en && slot->price >= (float)r->aoff_lim) {
            on = false;
            reason = RELAY_RSN_ALWAYS_OFF;
        }
    }

    /* Track when the relay transitions to ON */
    if (on && !st->on)
        st->on_since = t;
    else if (!on)
        st->on_since = 0;
    st->on = on;
    return reason;
}

void relay_simulate(const hour_slot_t *slots, int count,
                    const relay_rules_t *rules, relay_state_t st,
                    bool *out_on)
{
    for (int i = 0; i < count; i++) {
        relay_decide(&slots[i], slots[i].ts, rules, &st);
        out_on[i] = st.on;
    }
}

const char *relay_reason_str(relay_rsn_t r)
{
    static const char *names[] = {
        "No data", "Schedule", "Always ON", "Min-run", "Always OFF"
    };
    return (r >= 0 && r < (int)(sizeof(names) / sizeof(names[0]))) ? names[r] : "?";
}
