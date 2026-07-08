idf_component_register(SRCS "main.c" "hello_web.c" "web_platform.c"
                    INCLUDE_DIRS "."
                    REQUIRES wifi_manager ota_manager file_manager led_task sd_card sd_logger cjson
                    PRIV_REQUIRES esp_driver_gpio esp_http_server esp_app_format littlefs log esp_system esp_timer heap)

littlefs_create_partition_image(storage ../data FLASH_IN_PROJECT)
