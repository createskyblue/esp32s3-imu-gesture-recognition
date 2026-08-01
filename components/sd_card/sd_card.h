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
 */
typedef struct {
    const char *mount_point;      /* default: "/sdcard" */
    int mosi_io;                  /* default: 11  */
    int sclk_io;                  /* default: 12  */
    int miso_io;                  /* default: 13  */
    int cs_io;                    /* default: 10  */
    int host_id;                  /* default: SPI2_HOST */
    uint32_t max_freq_khz;        /* default: 20000 */
    uint8_t max_open_files;       /* default: 8 */
    uint16_t allocation_unit_size;/* default: 16 * 1024 */
} sd_card_config_t;

/** Initialize SD card over SPI using the template default configuration. */
esp_err_t sd_card_init(void);

/**
 * Initialize SD card over SPI using caller-provided configuration.
 * Mounts FAT at the configured mount point; no listing or self-test is run.
 */
esp_err_t sd_card_init_with_config(const sd_card_config_t *config);

#ifdef __cplusplus
}
#endif
