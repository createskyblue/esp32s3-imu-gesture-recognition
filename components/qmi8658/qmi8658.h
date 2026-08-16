#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** QMI8658 6-axis IMU I2C address (SA0 low on 立创实战派 ESP32-S3). */
#define QMI8658_I2C_ADDR 0x6A

/** Raw 16-bit accelerometer readings (device LSB, ±4 g range by default). */
typedef struct {
    int16_t acc_x;
    int16_t acc_y;
    int16_t acc_z;
} qmi8658_accel_raw_t;

/**
 * Initialize QMI8658 on an already-created I2C master bus.
 * Configures accelerometer ±4 g / 250 Hz and gyroscope 512 dps / 250 Hz
 * (same registers as the LCKFB official demo).
 */
esp_err_t qmi8658_init(i2c_master_bus_handle_t bus);

/** Read one raw accelerometer sample (X/Y/Z). */
esp_err_t qmi8658_read_accel_raw(qmi8658_accel_raw_t *out);

/** Convert a raw ±4 g sample to g (8192 LSB/g). */
float qmi8658_raw_to_g(int16_t raw);

#ifdef __cplusplus
}
#endif
