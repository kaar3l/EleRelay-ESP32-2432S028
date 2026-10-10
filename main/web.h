#pragma once
/*
 * Web UI: price table (public) and settings pages (HTTP Basic login,
 * cross-site POSTs rejected). In setup-AP mode only the WiFi form is served.
 */
#include "esp_http_server.h"

httpd_handle_t web_start(void);
