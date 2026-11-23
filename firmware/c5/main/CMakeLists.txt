idf_component_register(SRCS "main.c" "ble_test.c" "http_status.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_wifi nvs_flash esp_timer esp_netif lwip bt esp_http_server esp_https_server)
