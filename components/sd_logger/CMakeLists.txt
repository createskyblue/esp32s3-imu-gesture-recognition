idf_component_register(SRCS "sd_logger.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES freertos log esp_timer)
