idf_component_register(SRCS "wifi_manager.c"
                    INCLUDE_DIRS "."
                    REQUIRES led_task sd_logger cjson
                    PRIV_REQUIRES freertos esp_wifi esp_netif nvs_flash esp_timer lwip log)
