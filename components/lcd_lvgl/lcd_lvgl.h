#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Screen builder hook: called from the display task once LVGL is fully
 * initialized (tick + display + buffers ready), before the timer loop
 * starts. The callback runs in the LVGL/display task context and should
 * create the application's root UI on lv_scr_act(). A NULL builder keeps
 * the built-in demo screen.
 */
typedef void (*lcd_lvgl_screen_builder_t)(void);

/**
 * Start the 立创实战派 ESP32-S3 LCD (ST7789) + LVGL 9.5 display task.
 *
 * Initializes I2C + PCA9557 (LCD CS is driven by the IO expander),
 * SPI bus, ST7789 panel, LEDC backlight, then creates a FreeRTOS task
 * that runs the LVGL timer loop and shows a demo screen.
 *
 * Safe to call once from app_main; the display task tolerates a missing
 * panel by logging and deleting itself (the rest of the system keeps running).
 */
esp_err_t lcd_lvgl_start(void);

/** Register the application screen builder (called from the display task). */
void lcd_lvgl_set_screen_builder(lcd_lvgl_screen_builder_t builder);

/**
 * Get the I2C master bus handle created by the LCD driver, so other
 * peripherals on the same bus (e.g. the QMI8658 IMU) can be added.
 */
esp_err_t lcd_lvgl_get_i2c_bus(i2c_master_bus_handle_t *bus);

#ifdef __cplusplus
}
#endif