#include "qmi8658.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "QMI8658";

#define QMI8658_WHO_AM_I_VALUE 0x05u
#define QMI8658_RAW_TO_G_4G   8192.0f   /* LSB per g at ±4 g range */

/* Register map (subset used by this driver) */
#define QMI8658_REG_WHO_AM_I   0x00u
#define QMI8658_REG_CTRL1      0x02u
#define QMI8658_REG_CTRL2      0x03u
#define QMI8658_REG_CTRL3      0x04u
#define QMI8658_REG_CTRL7      0x08u
#define QMI8658_REG_AX_L       0x35u
#define QMI8658_REG_AX_H       0x36u
#define QMI8658_REG_AY_L       0x37u
#define QMI8658_REG_AY_H       0x38u
#define QMI8658_REG_AZ_L       0x39u
#define QMI8658_REG_AZ_H       0x3Au
#define QMI8658_REG_RESET      0x60u

/* CTRL1: enable address auto-increment for multi-byte reads */
#define QMI8658_CTRL1_AUTO_INC 0x40u
/* CTRL7: enable accelerometer + gyroscope */
#define QMI8658_CTRL7_ENABLE   0x03u
/* CTRL2: ACC ±4g @ 250 Hz (LCKFB demo value) */
#define QMI8658_CTRL2_ACC_4G_250HZ 0x95u
/* CTRL3: GYR 512dps @ 250 Hz (LCKFB demo value) */
#define QMI8658_CTRL3_GYR_512_250HZ 0xD5u

static i2c_master_dev_handle_t s_dev = NULL;

static esp_err_t qmi8658_reg_read(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, data, len, pdMS_TO_TICKS(100));
}

static esp_err_t qmi8658_reg_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), pdMS_TO_TICKS(100));
}

esp_err_t qmi8658_init(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = QMI8658_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &s_dev),
                        TAG, "add QMI8658 device");

    uint8_t id = 0;
    for (int i = 0; i < 5; i++) {
        if (qmi8658_reg_read(QMI8658_REG_WHO_AM_I, &id, 1) == ESP_OK &&
            id == QMI8658_WHO_AM_I_VALUE) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (id != QMI8658_WHO_AM_I_VALUE) {
        ESP_LOGE(TAG, "WHO_AM_I mismatch: got 0x%02X", id);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "QMI8658 found (WHO_AM_I=0x%02X)", id);

    ESP_RETURN_ON_ERROR(qmi8658_reg_write(QMI8658_REG_RESET, 0xB0), TAG, "reset");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(qmi8658_reg_write(QMI8658_REG_CTRL1, QMI8658_CTRL1_AUTO_INC), TAG, "CTRL1");
    ESP_RETURN_ON_ERROR(qmi8658_reg_write(QMI8658_REG_CTRL7, QMI8658_CTRL7_ENABLE), TAG, "CTRL7");
    ESP_RETURN_ON_ERROR(qmi8658_reg_write(QMI8658_REG_CTRL2, QMI8658_CTRL2_ACC_4G_250HZ), TAG, "CTRL2");
    ESP_RETURN_ON_ERROR(qmi8658_reg_write(QMI8658_REG_CTRL3, QMI8658_CTRL3_GYR_512_250HZ), TAG, "CTRL3");
    ESP_LOGI(TAG, "QMI8658 configured: ACC +-4g/250Hz, GYR 512dps/250Hz");
    return ESP_OK;
}

esp_err_t qmi8658_read_accel_raw(qmi8658_accel_raw_t *out)
{
    if (s_dev == NULL || out == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[6];
    ESP_RETURN_ON_ERROR(qmi8658_reg_read(QMI8658_REG_AX_L, buf, 6),
                        TAG, "read accel registers");
    out->acc_x = (int16_t)((uint16_t)buf[1] << 8 | buf[0]);
    out->acc_y = (int16_t)((uint16_t)buf[3] << 8 | buf[2]);
    out->acc_z = (int16_t)((uint16_t)buf[5] << 8 | buf[4]);
    return ESP_OK;
}

float qmi8658_raw_to_g(int16_t raw)
{
    return (float)raw / QMI8658_RAW_TO_G_4G;
}
