#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"

#define BLUFI_PROVISIONING_TAG "BLUFI_PROV"
#define BLUFI_INFO(fmt, ...)   ESP_LOGI(BLUFI_PROVISIONING_TAG, fmt, ##__VA_ARGS__)
#define BLUFI_ERROR(fmt, ...)  ESP_LOGE(BLUFI_PROVISIONING_TAG, fmt, ##__VA_ARGS__)

/* BluFi protocol security: DH key negotiation + AES-256-CTR + CRC16.
 * Ported from the ESP-IDF blufi example (Unlicense OR CC0-1.0). */
void blufi_dh_negotiate_data_handler(uint8_t *data, int len,
                                     uint8_t **output_data, int *output_len,
                                     bool *need_free);
int blufi_aes_encrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len);
int blufi_aes_decrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len);
uint16_t blufi_crc_checksum(uint8_t iv8, uint8_t *data, int len);
int blufi_security_init(void);
void blufi_security_deinit(void);
