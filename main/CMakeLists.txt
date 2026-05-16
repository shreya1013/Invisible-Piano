idf_component_register(SRCS "main.c"
                        INCLUDE_DIRS "."
                        REQUIRES espressif__esp32-camera esp_wifi esp_http_server nvs_flash esp_netif esp_timer)