idf_component_register(SRCS "network.c" "network_manager.c" "main.c" "camera.c" "wifi.c" "network.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp32-camera
                    PRIV_REQUIRES
                        esp_wifi
                        nvs_flash
                        esp_event
                        esp_netif
                        esp_http_server
                        esp_http_client
                        esp_timer
                        esp_system
                        esp_psram
                        esp_driver_gpio
                        lwip
                        heap)