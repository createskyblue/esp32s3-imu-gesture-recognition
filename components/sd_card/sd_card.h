#pragma once

#include "esp_err.h"

/**
 * Initialize SD card over SPI, mount FAT filesystem at "/sdcard",
 * list root directory, and run a simple read/write test.
 *
 * Pins: MOSI=IO11, SCLK=IO12, MISO=IO13, CS=IO10, CD=IO14
 */
esp_err_t sd_card_init(void);
