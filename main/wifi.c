/*
 * WiFi station / setup access point, NTP — see wifi.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "lwip/inet.h"
#include "display.h"
#include "wifi.h"

static const char *TAG = "wifi";

#define WIFI_MAX_RETRY   10

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_ASSOC_BIT     BIT2

char g_ip[20]              = "?.?.?.?";
bool g_ap_mode             = false;
char g_wifi_ssid[CRED_LEN] = {0};

static EventGroupHandle_t s_wifi_eg;
static int s_retry_num = 0;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        xEventGroupSetBits(s_wifi_eg, WIFI_ASSOC_BIT);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_eg, WIFI_CONNECTED_BIT);
        if (s_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect(); s_retry_num++;
            ESP_LOGW(TAG, "WiFi retry %d/%d", s_retry_num, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "WiFi failed after %d retries — wifi_watchdog_task will keep retrying", WIFI_MAX_RETRY);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        snprintf(g_ip, sizeof(g_ip), IPSTR, IP2STR(&evt->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "WiFi connected — IP: %s", g_ip);
    }
}

void wifi_init(void)
{
    s_wifi_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));
}

bool wifi_start_sta(const char *ssid, const char *pass)
{
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();

    /* Set hostname to EleRelay-XXYYZZ (last 3 bytes of MAC) */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char hostname[24];
    snprintf(hostname, sizeof(hostname), "EleRelay-%02X%02X%02X",
             mac[3], mac[4], mac[5]);
    esp_netif_set_hostname(sta, hostname);
    ESP_LOGI(TAG, "Hostname: %s", hostname);

    if (g_cfg.ip_mode == 1 && g_cfg.static_ip[0]) {
        esp_netif_dhcpc_stop(sta);
        esp_netif_ip_info_t ip_info = {0};
        ip4_addr_t addr;
        if (ip4addr_aton(g_cfg.static_ip, &addr))   ip_info.ip.addr      = addr.addr;
        if (ip4addr_aton(g_cfg.static_mask, &addr)) ip_info.netmask.addr = addr.addr;
        if (ip4addr_aton(g_cfg.static_gw, &addr))   ip_info.gw.addr      = addr.addr;
        esp_netif_set_ip_info(sta, &ip_info);
        if (g_cfg.static_dns[0] && ip4addr_aton(g_cfg.static_dns, &addr)) {
            esp_netif_dns_info_t dns_info = {0};
            dns_info.ip.type     = ESP_IPADDR_TYPE_V4;
            dns_info.ip.u_addr.ip4.addr = addr.addr;
            esp_netif_set_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns_info);
        }
        ESP_LOGI(TAG, "Static IP: %s / %s gw %s dns %s",
                 g_cfg.static_ip, g_cfg.static_mask, g_cfg.static_gw, g_cfg.static_dns);
    }

    wifi_config_t wcfg = {0};
    strlcpy((char *)wcfg.sta.ssid,     ssid, sizeof(wcfg.sta.ssid));
    strlcpy((char *)wcfg.sta.password, pass, sizeof(wcfg.sta.password));
    if (strlen(pass) > 0) wcfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_eg, WIFI_ASSOC_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, portMAX_DELAY);
    if (bits & WIFI_FAIL_BIT) return false;

    display_status("Connecting to WiFi",
                    (g_cfg.ip_mode == 1) ? "Setting static IP..." : "Getting IP via DHCP...");

    bits = xEventGroupWaitBits(
        s_wifi_eg, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, portMAX_DELAY);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

void wifi_start_ap(void)
{
    esp_wifi_stop();
    esp_netif_create_default_wifi_ap();
    wifi_config_t ap_cfg = {0};
    strlcpy((char *)ap_cfg.ap.ssid, AP_SSID, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(AP_SSID);
    ap_cfg.ap.channel  = 6;
    strlcpy((char *)ap_cfg.ap.password, g_cfg.dev_pass, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_cfg.ap.max_connection = 4;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    snprintf(g_ip, sizeof(g_ip), "192.168.4.1");
    ESP_LOGI(TAG, "AP started — SSID: %s (WPA2, password on LCD)  IP: 192.168.4.1", AP_SSID);
}

void ntp_sync(void)
{
    setenv("TZ", g_cfg.tz_str, 1);
    tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, g_cfg.ntp_server);
    esp_sntp_setservername(1, "time.cloudflare.com");
    esp_sntp_init();
    time_t now = 0; struct tm ti = {0};
    for (int i = 0; ti.tm_year < (2020-1900) && i < 40; i++) {
        ESP_LOGI(TAG, "NTP sync... (%d)", i+1);
        vTaskDelay(pdMS_TO_TICKS(2000));
        time(&now); localtime_r(&now, &ti);
    }
    char buf[32]; strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &ti);
    ESP_LOGI(TAG, "Time: %s", buf);
}

/* wifi_event_handler stops retrying after WIFI_MAX_RETRY consecutive
 * failures. This task notices when STA is still disconnected and kicks
 * off another round of retries, so a reboot of the AP doesn't strand the
 * device permanently. */
void wifi_watchdog_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        if (g_ap_mode) continue;
        EventBits_t bits = xEventGroupGetBits(s_wifi_eg);
        if (!(bits & WIFI_CONNECTED_BIT)) {
            ESP_LOGW(TAG, "WiFi still disconnected — retrying");
            s_retry_num = 0;
            xEventGroupClearBits(s_wifi_eg, WIFI_FAIL_BIT);
            esp_wifi_connect();
        }
    }
}

/* Only started when NVS credentials exist, so first-boot setup is unaffected */
void ap_reconnect_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5 * 60 * 1000));
        ESP_LOGI(TAG, "AP mode: retrying STA connection");
        esp_restart();
    }
}
