#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Gesture-capture demo (accelerometer based).
 *
 * UI: three live accelerometer waveforms (X/Y/Z), per-axis max/min on the
 * right, and a bottom row of four buttons: A / B / C (gesture classes) and
 * Inference (reserved for later).
 *
 * Flow: tap BOOT -> record 2 s @ 50 Hz -> plot + append one interleaved
 * x,y,z,x,y,z,... row to the CSV of the currently selected gesture.
 * A fresh set of CSV files (gesture_A/B/C_<boot>.csv) is created at boot.
 */

/** Called from the LVGL display task (via lcd_lvgl_set_screen_builder). */
void gesture_demo_ui_create(void);

/** Init IMU + BOOT button + CSV files + capture task. Call after lcd_lvgl_start(). */
esp_err_t gesture_demo_start(void);

#ifdef __cplusplus
}
#endif
