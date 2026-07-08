idf_component_register(SRCS "main.c" "hello_web.c"
                    INCLUDE_DIRS "."
                    REQUIRES web_platform led_task sd_card sd_logger
                    PRIV_REQUIRES esp_driver_gpio)

littlefs_create_partition_image(storage ../data FLASH_IN_PROJECT)
