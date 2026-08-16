#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif