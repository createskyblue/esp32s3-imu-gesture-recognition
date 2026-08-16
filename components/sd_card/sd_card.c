#include "sd_card.h"

#include <stdio.h>
#include <string.h>

#include "driver/sdmmc_host.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

static const char *TAG = "SD";

static sdmmc_card_t *s_card = NULL;

/* ── 立创实战派 ESP32-S3 board defaults: SDMMC 1-bit ───────────────
 * SD card is wired to the SDMMC peripheral (not SPI):
 *   CLK=47, CMD=48, D0=21  (see lckfb example 03-micro_sd)            */
static const sd_card_config_t SD_CARD_DEFAULT_CONFIG = {
    .mount_point = SD_CARD_DEFAULT_MOUNT_POINT,
    .clk_io = 47,
    .cmd_io = 48,
    .d0_io = 21,
    .max_freq_khz = 20000,          /* 20 MHz */
    .max_open_files = 8,
    .allocation_unit_size = 16 * 1024,
};

static esp_err_t config_copy(sd_card_config_t *dest,
                             const sd_card_config_t *source)
{
    *dest = SD_CARD_DEFAULT_CONFIG;

    if (source == NULL) return ESP_OK;
    if (source->mount_point != NULL && source->mount_point[0] != '\0')
        dest->mount_point = source->mount_point;
    if (source->clk_io > 0) dest->clk_io = source->clk_io;
    if (source->cmd_io > 0) dest->cmd_io = source->cmd_io;
    if (source->d0_io > 0)  dest->d0_io = source->d0_io;
    if (source->max_freq_khz > 0) dest->max_freq_khz = source->max_freq_khz;
    if (source->max_open_files > 0) dest->max_open_files = source->max_open_files;
    if (source->allocation_unit_size > 0)
        dest->allocation_unit_size = source->allocation_unit_size;

    if (dest->mount_point == NULL || dest->mount_point[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }
    if (dest->clk_io < 0 || dest->cmd_io < 0 || dest->d0_io < 0 ||
        dest->max_freq_khz == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t sd_card_init_with_config(const sd_card_config_t *config)
{
    sd_card_config_t cfg;
    esp_err_t err = config_copy(&cfg, config);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "Initializing SD card (SDMMC 1-bit mode)...");
    ESP_LOGI(TAG, "  CLK=IO%d, CMD=IO%d, D0=IO%d",
             cfg.clk_io, cfg.cmd_io, cfg.d0_io);

    /* SDMMC host (1-bit width, internal pull-ups on) */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = cfg.max_freq_khz;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;          /* 1-bit SD mode */
    slot_config.clk = cfg.clk_io;
    slot_config.cmd = cfg.cmd_io;
    slot_config.d0 = cfg.d0_io;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_mount_config_t mount_cfg = {
        .max_files = cfg.max_open_files,
        .format_if_mount_failed = false,
        .allocation_unit_size = cfg.allocation_unit_size,
    };

    /* Wait for the SD card to stabilize after power-on */
    vTaskDelay(pdMS_TO_TICKS(1000));

    err = esp_vfs_fat_sdmmc_mount(cfg.mount_point, &host, &slot_config,
                                  &mount_cfg, &s_card);
    if (err != ESP_OK) {
        if (err == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount FAT filesystem. Is the SD card formatted as FAT?");
        } else {
            ESP_LOGE(TAG, "Failed to init SD card: %s", esp_err_to_name(err));
        }
        return err;
    }

    /* Print card info */
    sdmmc_card_print_info(stdout, s_card);

    ESP_LOGI(TAG, "SD card ready at %s", cfg.mount_point);
    return ESP_OK;
}

esp_err_t sd_card_init(void)
{
    return sd_card_init_with_config(&SD_CARD_DEFAULT_CONFIG);
}