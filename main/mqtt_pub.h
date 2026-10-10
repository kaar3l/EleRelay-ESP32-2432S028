#pragma once
/*
 * MQTT publishing of price and relay state (settings from g_cfg).
 */

void mqtt_pub_init(void);

/* (Re)start the client with the current g_cfg settings; stops it when
 * MQTT is disabled or no broker is set. Safe to call from any task. */
void mqtt_start(void);

/* Publish with retain=1; no-op when MQTT is not running */
void mqtt_publish(const char *topic, const char *payload);
