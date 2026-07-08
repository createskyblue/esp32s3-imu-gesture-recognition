idf_component_register(SRCS "main.c" "web_server.c" "led_task.c" "sd_card.c" "sd_logger.c" "file_manager.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp_driver_gpio esp_timer esp_wifi esp_http_server esp_rom nvs_flash littlefs esp_app_format app_update esp_partition esp_http_client esp_service esp_ota_service fatfs esp_driver_sdspi)

littlefs_create_partition_image(storage ../data FLASH_IN_PROJECT)
