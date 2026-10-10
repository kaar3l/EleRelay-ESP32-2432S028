#pragma once
/*
 * WiFi station / setup access point, NTP, and reconnect watchdogs.
 */
#include <stdbool.h>
#include "settings.h"

#define AP_SSID          "EleRelay-Setup"

extern char g_ip[20];                  /* current IP as text */
extern bool g_ap_mode;                 /* true when running the setup AP */
extern char g_wifi_ssid[CRED_LEN];     /* configured station SSID */

/* netif, event loop and WiFi driver init + event handlers */
void wifi_init(void);

/* Connect as station (static IP from g_cfg if set). Blocks until an IP is
 * obtained (true) or the retries are exhausted (false). */
bool wifi_start_sta(const char *ssid, const char *pass);

/* Start the WPA2 setup AP (password = g_cfg.dev_pass) */
void wifi_start_ap(void);

/* Apply g_cfg.tz_str, start SNTP and wait up to 80 s for a valid time */
void ntp_sync(void);

/* Station mode: re-kicks the connection every 30 s while disconnected */
void wifi_watchdog_task(void *arg);

/* AP mode: reboot every 5 minutes to retry the stored network */
void ap_reconnect_task(void *arg);
