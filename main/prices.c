/*
 * Elering NPS price API — see prices.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "prices.h"

static const char *TAG = "prices";

#define HTTP_BUF_SIZE    (24 * 1024)

typedef struct {
    char *buf;
    int   len;
} http_buf_t;

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    http_buf_t *hb = (http_buf_t *)evt->user_data;
    if (!hb || !hb->buf) return ESP_OK;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (hb->len + evt->data_len < HTTP_BUF_SIZE - 1) {
            memcpy(hb->buf + hb->len, evt->data, evt->data_len);
            hb->len += evt->data_len;
        }
    } else if (evt->event_id == HTTP_EVENT_ON_FINISH) {
        hb->buf[hb->len] = '\0';
    }
    return ESP_OK;
}

bool inet_check(void)
{
    esp_http_client_config_t cfg = {
        .url               = "https://dashboard.elering.ee/",
        .method            = HTTP_METHOD_HEAD,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t err    = esp_http_client_perform(client);
    int       status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return err == ESP_OK && status >= 200 && status < 400;
}

esp_err_t prices_fetch(hour_slot_t *out, int max, int *count)
{
    *count = 0;
    time_t now; time(&now);
    time_t win_start = (now / 3600) * 3600;
    time_t win_end   = win_start + (MAX_SLOTS / 4) * 3600;

    struct tm ts, te;
    gmtime_r(&win_start, &ts);
    gmtime_r(&win_end,   &te);

    char url[300];
    snprintf(url, sizeof(url),
        "https://dashboard.elering.ee/api/nps/price"
        "?fields=ee"
        "&start=%04d-%02d-%02dT%02d:00:00.000Z"
        "&end=%04d-%02d-%02dT%02d:00:00.000Z",
        ts.tm_year+1900, ts.tm_mon+1, ts.tm_mday, ts.tm_hour,
        te.tm_year+1900, te.tm_mon+1, te.tm_mday, te.tm_hour);
    ESP_LOGI(TAG, "GET %s", url);

    http_buf_t hb = { .buf = calloc(1, HTTP_BUF_SIZE), .len = 0 };
    if (!hb.buf) return ESP_ERR_NO_MEM;

    esp_http_client_config_t cfg = {
        .url               = url,
        .event_handler     = http_event_cb,
        .user_data         = &hb,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 20000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t err    = esp_http_client_perform(client);
    int       status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "HTTP %s / %d", esp_err_to_name(err), status);
        free(hb.buf);
        return ESP_FAIL;
    }

    hb.buf[hb.len] = '\0';
    cJSON *root = cJSON_Parse(hb.buf);
    free(hb.buf);
    if (!root) { ESP_LOGE(TAG, "JSON parse failed"); return ESP_FAIL; }

    cJSON *ee = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "data"), "ee");
    if (!cJSON_IsArray(ee)) {
        ESP_LOGE(TAG, "No data.ee array"); cJSON_Delete(root); return ESP_FAIL;
    }

    /* Store each 15-min slot directly — no hourly aggregation */
    int n = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, ee) {
        cJSON *ts_j = cJSON_GetObjectItem(item, "timestamp");
        cJSON *pr_j = cJSON_GetObjectItem(item, "price");
        if (!cJSON_IsNumber(ts_j) || !cJSON_IsNumber(pr_j)) continue;
        time_t slot_ts = (time_t)ts_j->valuedouble;
        if (slot_ts < win_start || n >= max) continue;
        out[n].ts    = slot_ts;
        out[n].price = (float)pr_j->valuedouble;
        out[n].cheap = false;
        n++;
    }
    cJSON_Delete(root);

    /* Sort chronologically (API order may vary) */
    for (int i = 0; i < n - 1; i++)
        for (int j = i + 1; j < n; j++)
            if (out[j].ts < out[i].ts) {
                hour_slot_t t = out[i]; out[i] = out[j]; out[j] = t;
            }

    *count = n;
    ESP_LOGI(TAG, "Parsed %d × 15-min slots", n);
    return ESP_OK;
}
