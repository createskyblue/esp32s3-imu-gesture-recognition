idf_component_register(SRCS "main.c" "web_platform.c"
                         "app_storage.c" "wifi_config_store.c"
                    INCLUDE_DIRS "."
                    REQUIRES json wifi_manager ota_manager file_manager led_task cjson
                    PRIV_REQUIRES esp_http_server esp_app_format littlefs log esp_system esp_timer heap freertos)

littlefs_create_partition_image(storage ../data FLASH_IN_PROJECT)
