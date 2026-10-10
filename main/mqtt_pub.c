/*
 * MQTT publishing — see mqtt_pub.h.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "settings.h"
#include "mqtt_pub.h"

static const char *TAG = "mqtt";

/* Guards s_mqtt_client against being destroyed (mqtt_start from the web
 * handler) while another task is publishing through it. */
static SemaphoreHandle_t s_mqtt_mutex;
static esp_mqtt_client_handle_t s_mqtt_client = NULL;

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    if      (id == MQTT_EVENT_CONNECTED)    ESP_LOGI(TAG, "MQTT connected");
    else if (id == MQTT_EVENT_DISCONNECTED) ESP_LOGW(TAG, "MQTT disconnected");
    else if (id == MQTT_EVENT_ERROR)        ESP_LOGE(TAG, "MQTT error");
}

void mqtt_pub_init(void)
{
    s_mqtt_mutex = xSemaphoreCreateMutex();
}

void mqtt_start(void)
{
    xSemaphoreTake(s_mqtt_mutex, portMAX_DELAY);
    /* Tear down any existing client */
    if (s_mqtt_client) {
        esp_mqtt_client_stop(s_mqtt_client);
        esp_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
    }
    if (!g_cfg.mqtt_enabled || g_cfg.mqtt_host[0] == '\0') {
        xSemaphoreGive(s_mqtt_mutex);
        return;
    }

    char uri[96];
    snprintf(uri, sizeof(uri), "mqtt://%s:%d", g_cfg.mqtt_host, g_cfg.mqtt_port);
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
    };
    s_mqtt_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt_client);
    xSemaphoreGive(s_mqtt_mutex);
    ESP_LOGI(TAG, "MQTT → %s", uri);
}

void mqtt_publish(const char *topic, const char *payload)
{
    if (!topic || topic[0] == '\0') return;
    xSemaphoreTake(s_mqtt_mutex, portMAX_DELAY);
    if (s_mqtt_client && g_cfg.mqtt_enabled)
        esp_mqtt_client_publish(s_mqtt_client, topic, payload, 0, /*qos*/0, /*retain*/1);
    xSemaphoreGive(s_mqtt_mutex);
}
