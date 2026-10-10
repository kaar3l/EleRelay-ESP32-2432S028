#pragma once
/*
 * Shared application state and services provided by main.c.
 */
#include <stdbool.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "scheduler.h"

typedef struct {
    hour_slot_t   slots[MAX_SLOTS];   /* fetched prices, sorted by ts */
    int           count;
    relay_state_t relay;              /* actual relay output state */
    relay_rsn_t   relay_reason;
    time_t        last_fetch;         /* 0 = never */
    time_t        last_slot_ts;       /* end of the last fetched slot */
    volatile bool inet_ok;
} app_state_t;

/* Guarded by g_state_mutex, which also serializes writes to g_cfg */
extern app_state_t       g_state;
extern SemaphoreHandle_t g_state_mutex;

/* Relay rules from the current g_cfg */
relay_rules_t app_relay_rules(void);

/* Re-run mark_cheap_hours() on g_state with the current g_cfg.
 * Caller must hold g_state_mutex. */
void app_recompute_cheap(void);

/* Evaluate the relay for "now", drive the GPIO/LEDs and publish MQTT */
void update_relay(void);

/* Ask the network task to fetch prices (returns immediately). A request
 * while a fetch is already running is ignored. */
void app_request_fetch(void);
bool app_fetch_busy(void);

/* Reboot after ~1.2 s, so an HTTP response can still be sent */
void app_restart_later(void);

/* Snapshot of the prices plus the forecast relay state per slot */
typedef struct {
    hour_slot_t slots[MAX_SLOTS];
    bool        on[MAX_SLOTS];   /* forecast; valid from index first */
    int         count;
    int         first;           /* first slot that has not ended yet */
    bool        relay_on;        /* actual relay state now */
    time_t      last_fetch;
} app_forecast_t;

void app_forecast(app_forecast_t *f, time_t now);
