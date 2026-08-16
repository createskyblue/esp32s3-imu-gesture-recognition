idf_component_register(SRCS "lcd_lvgl.c"
                    INCLUDE_DIRS "."
                    REQUIRES lvgl esp_lcd esp_driver_spi esp_driver_i2c esp_driver_ledc esp_driver_gpio esp_timer freertos log heap)

# LVGL's esp.cmake compiles with LV_CONF_INCLUDE_SIMPLE; relax only the
# type-limits warning for the LVGL target so IDF's -Werror does not break it
# (same workaround as the ESP32_PAR20_4H_IDF reference project).
set_property(TARGET __idf_lvgl__lvgl APPEND PROPERTY COMPILE_OPTIONS
  -Wno-error=type-limits)