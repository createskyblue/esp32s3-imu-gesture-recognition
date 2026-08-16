idf_component_register(SRCS "main.c" "gesture_demo.c" "web_platform.c"
                         "app_storage.c" "wifi_config_store.c"
                    INCLUDE_DIRS "."
                    REQUIRES json qmi8658 esp_driver_gpio nvs_flash wifi_manager ota_manager file_manager led_task cjson ble_host blufi_provisioning ble_echo ble_host_test lcd_lvgl gesture_classifier sd_card
                    PRIV_REQUIRES esp_http_server esp_app_format littlefs log esp_system esp_timer heap freertos)

littlefs_create_partition_image(storage ../data FLASH_IN_PROJECT)