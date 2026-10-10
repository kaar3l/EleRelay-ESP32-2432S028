#pragma once
/*
 * Runtime settings (persisted in NVS, defaults from Kconfig) and WiFi
 * credentials. g_cfg is written by the web handlers / touch UI while holding
 * g_state_mutex (see app.h); single-int reads elsewhere are not locked.
 */
#include <stdbool.h>
#include "esp_err.h"

#define CRED_LEN         64
#define STR_LEN          64
#define IP_STR_LEN       16

/* Web UI login user. The password (g_cfg.dev_pass) is also the setup-AP WPA2 key. */
#define AUTH_USER        "admin"
#define DEV_PASS_MIN     8      /* WPA2 minimum */
#define DEV_PASS_MAX     32     /* WPA2 allows 63; 32 still fits on the LCD info page */

/* Always-ON/OFF limit range in EUR/MWh (= 0.1 c/kWh): -100 .. 1000 c/kWh */
#define PRICE_LIM_MIN    (-1000)
#define PRICE_LIM_MAX    10000

typedef struct {
    /* Relay schedule */
    int  hours_window;          /* 2-24 */
    int  cheap_hours;           /* 1 .. window-1 */
    bool relay_inv;             /* invert relay logic */
    int  fetch_hour;            /* 0-23 */
    int  min_run_minutes;       /* 0 = disabled */
    bool always_on_en;          /* force ON below limit */
    int  always_on_limit;       /* EUR/MWh (0 = 0 c/kWh) */
    bool always_off_en;         /* force OFF above limit */
    int  always_off_limit;      /* EUR/MWh (200 = 20 c/kWh) */

    /* Device */
    int  lang;                  /* LANG_EN / LANG_ET */
    int  backlight_pct;         /* 0-100 % */
    int  inet_check_min;        /* 0 = disabled */
    char ntp_server[STR_LEN];
    char tz_str[STR_LEN];
    char dev_pass[DEV_PASS_MAX + 1];

    /* MQTT */
    bool mqtt_enabled;
    char mqtt_host[STR_LEN];
    int  mqtt_port;
    char mqtt_topic_price[STR_LEN];
    char mqtt_topic_relay[STR_LEN];

    /* Network */
    int  ip_mode;               /* 0 = DHCP, 1 = static */
    char static_ip[IP_STR_LEN];
    char static_mask[IP_STR_LEN];
    char static_gw[IP_STR_LEN];
    char static_dns[IP_STR_LEN];
} app_settings_t;

extern app_settings_t g_cfg;

/* clamp an int to [lo, hi] */
static inline int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void      settings_load(void);
esp_err_t settings_save(void);

esp_err_t creds_load(char *ssid, char *pass);   /* buffers of CRED_LEN */
esp_err_t creds_save(const char *ssid, const char *pass);

/* Create a random device password on first boot. Must run before WiFi is
 * started (uses the bootloader entropy source). */
void      dev_pass_init(void);
esp_err_t dev_pass_save(void);
