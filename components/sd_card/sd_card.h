#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Default SD card mount point used by sd_card_init(). */
#define SD_CARD_DEFAULT_MOUNT_POINT "/sdcard"

/**
 * Caller-owned SD card configuration copied by sd_card_init_with_config().
 * Zeroed fields fall back to the template defaults shown in the comments.
 * The board default is 1-bit SDMMC (立创实战派 ESP32-S3: CLK=47, CMD=48, D0=21).
 */
typedef struct {
    const char *mount_point;      /* default: "/sdcard" */
    int clk_io;                   /* default: 47 */
    int cmd_io;                   /* default: 48 */
    int d0_io;                    /* default: 21 */
    uint32_t max_freq_khz;        /* default: 20000 (20 MHz) */
    uint8_t max_open_files;       /* default: 8 */
    uint16_t allocation_unit_size;/* default: 16 * 1024 */
} sd_card_config_t;

/** Initialize SD card over SDMMC (1-bit) using the template default config. */
esp_err_t sd_card_init(void);

/**
 * Initialize SD card over SDMMC (1-bit) using caller-provided configuration.
 * Mounts FAT at the configured mount point; no listing or self-test is run.
 */
esp_err_t sd_card_init_with_config(const sd_card_config_t *config);

#ifdef __cplusplus
}
#endif