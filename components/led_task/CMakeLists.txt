idf_component_register(SRCS "led_task.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp_driver_gpio freertos log)
