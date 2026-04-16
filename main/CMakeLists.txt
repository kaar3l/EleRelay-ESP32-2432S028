idf_component_register(
    SRCS "main.c" "display.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_http_client
        esp_http_server
        json
        lwip
        driver
        mbedtls
        esp-tls
        esp_lcd
        mqtt
        app_update
)
