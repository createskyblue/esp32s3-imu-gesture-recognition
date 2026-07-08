idf_component_register(SRCS "sd_card.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp_driver_gpio esp_driver_spi freertos log fatfs esp_driver_sdspi)
