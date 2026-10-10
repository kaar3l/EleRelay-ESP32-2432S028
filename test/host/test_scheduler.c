/*
 * Host unit tests for main/scheduler.c (no ESP-IDF needed).
 * Run: make -C test/host
 */
#include <stdio.h>
#include "scheduler.h"

static int s_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); s_fail++; } } while (0)

#define T0 1760000400   /* an hour boundary */

static void fill(hour_slot_t *s, int n, const float *prices)
{
    for (int i = 0; i < n; i++) {
        s[i].ts = T0 + i * SLOT_SECS;
        s[i].price = prices ? prices[i] : (float)((i * 37) % 100);
        s[i].cheap = false;
    }
}

static int count_cheap(const hour_slot_t *s, int from, int to)
{
    int c = 0;
    for (int i = from; i < to; i++) c += s[i].cheap;
    return c;
}

static void test_cheapest_slots_selected(void)
{
    /* 2 h window, 1 cheap hour: the 4 cheapest of 8 slots */
    float p[8] = { 50, 10, 60, 20, 70, 30, 80, 40 };
    hour_slot_t s[8];
    fill(s, 8, p);
    mark_cheap_hours(s, 8, 1, 2, 0);
    CHECK(s[1].cheap && s[3].cheap && s[5].cheap && s[7].cheap);
    CHECK(count_cheap(s, 0, 8) == 4);
}

static void test_partial_window_scaled(void)
{
    /* 12 h window, 6 cheap hours: full window gets 24 of 48 slots, a 5 h
     * leftover window gets 10 of 20 (not all 20) */
    hour_slot_t s[68];
    fill(s, 68, NULL);
    mark_cheap_hours(s, 68, 6, 12, 0);
    CHECK(count_cheap(s, 0, 48) == 24);
    CHECK(count_cheap(s, 48, 68) == 10);
}

static void test_min_run_blocks_contiguous(void)
{
    /* min run 60 min → cheap slots come in runs of at least 4 */
    hour_slot_t s[48];
    fill(s, 48, NULL);
    mark_cheap_hours(s, 48, 6, 12, 60);
    int run = 0;
    for (int i = 0; i <= 48; i++) {
        if (i < 48 && s[i].cheap) { run++; continue; }
        if (run) CHECK(run >= 4);
        run = 0;
    }
}

static void test_decide_priorities(void)
{
    relay_rules_t r = { .inv = false, .min_run_minutes = 0,
                        .aon_en = true, .aon_lim = 0,
                        .aoff_en = true, .aoff_lim = 200 };
    relay_state_t st = {0};
    hour_slot_t slot = { .ts = T0, .price = 100, .cheap = true };

    CHECK(relay_decide(&slot, T0, &r, &st) == RELAY_RSN_SCHEDULE && st.on);
    CHECK(st.on_since == T0);

    slot.cheap = false; slot.price = -5;            /* negative price → always ON */
    CHECK(relay_decide(&slot, T0, &r, &st) == RELAY_RSN_ALWAYS_ON && st.on);

    slot.cheap = true; slot.price = 250;            /* above OFF limit wins */
    CHECK(relay_decide(&slot, T0, &r, &st) == RELAY_RSN_ALWAYS_OFF && !st.on);
    CHECK(st.on_since == 0);

    CHECK(relay_decide(NULL, T0, &r, &st) == RELAY_RSN_NO_DATA && !st.on);

    r.inv = true; r.aon_en = r.aoff_en = false;
    slot.cheap = true; slot.price = 100;
    relay_decide(&slot, T0, &r, &st);
    CHECK(!st.on);                                  /* inverted: OFF when cheap */
}

static void test_min_run_holds(void)
{
    relay_rules_t r = { .min_run_minutes = 30 };
    relay_state_t st = {0};
    hour_slot_t cheap = { .ts = T0, .price = 10, .cheap = true };
    hour_slot_t exp   = { .ts = T0 + 900, .price = 90, .cheap = false };

    relay_decide(&cheap, T0, &r, &st);
    CHECK(relay_decide(&exp, T0 + 900, &r, &st) == RELAY_RSN_MIN_RUN && st.on);
    exp.ts = T0 + 1800;
    CHECK(relay_decide(&exp, T0 + 1800, &r, &st) == RELAY_RSN_SCHEDULE && !st.on);
}

/* The forecast must match stepping relay_decide slot by slot — the web
 * table, LCD bars and the real relay all use this one decision function. */
static void test_simulate_matches_decide(void)
{
    hour_slot_t s[96];
    fill(s, 96, NULL);
    mark_cheap_hours(s, 96, 4, 12, 45);
    relay_rules_t r = { .min_run_minutes = 45, .aon_en = true, .aon_lim = 5,
                        .aoff_en = true, .aoff_lim = 95 };
    relay_state_t start = { .on = true, .on_since = T0 - 600 };

    bool fc[96];
    relay_simulate(s, 96, &r, start, fc);

    relay_state_t st = start;
    for (int i = 0; i < 96; i++) {
        relay_decide(&s[i], s[i].ts, &r, &st);
        CHECK(fc[i] == st.on);
    }
}

int main(void)
{
    test_cheapest_slots_selected();
    test_partial_window_scaled();
    test_min_run_blocks_contiguous();
    test_decide_priorities();
    test_min_run_holds();
    test_simulate_matches_decide();
    if (s_fail) { printf("%d check(s) failed\n", s_fail); return 1; }
    printf("All scheduler tests passed\n");
    return 0;
}
