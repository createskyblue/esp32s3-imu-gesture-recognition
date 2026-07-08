idf_component_register(SRCS "web_platform.c"
                    INCLUDE_DIRS "."
                    REQUIRES wifi_manager ota_manager file_manager led_task cjson
                    PRIV_REQUIRES esp_http_server esp_app_format littlefs log esp_system esp_timer heap)
