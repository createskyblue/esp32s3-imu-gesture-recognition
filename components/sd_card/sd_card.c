#include "sd_card.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

static const char *TAG = "SD";

static sdmmc_card_t *s_card = NULL;

/* ── template defaults ──────────────────────────────────────────────── */
static const sd_card_config_t SD_CARD_DEFAULT_CONFIG = {
    .mount_point = SD_CARD_DEFAULT_MOUNT_POINT,
    .mosi_io = 11,
    .sclk_io = 12,
    .miso_io = 13,
    .cs_io = 10,
    .host_id = SPI2_HOST,
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
    if (source->mosi_io > 0) dest->mosi_io = source->mosi_io;
    if (source->sclk_io > 0) dest->sclk_io = source->sclk_io;
    if (source->miso_io > 0) dest->miso_io = source->miso_io;
    if (source->cs_io > 0)   dest->cs_io = source->cs_io;
    if (source->host_id != 0) dest->host_id = source->host_id;
    if (source->max_freq_khz > 0) dest->max_freq_khz = source->max_freq_khz;
    if (source->max_open_files > 0) dest->max_open_files = source->max_open_files;
    if (source->allocation_unit_size > 0)
        dest->allocation_unit_size = source->allocation_unit_size;

    if (dest->mount_point == NULL || dest->mount_point[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }
    if (dest->mosi_io < 0 || dest->sclk_io < 0 || dest->miso_io < 0 ||
        dest->cs_io < 0 || dest->max_freq_khz == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t sd_card_init_with_config(const sd_card_config_t *config)
{
    sd_card_config_t cfg;
    esp_err_t err = config_copy(&cfg, config);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "Initializing SD card (SPI mode)...");
    ESP_LOGI(TAG, "  MOSI=IO%d, SCLK=IO%d, MISO=IO%d, CS=IO%d",
             cfg.mosi_io, cfg.sclk_io, cfg.miso_io, cfg.cs_io);

    /* Enable internal pull-ups on SPI pins */
    gpio_set_pull_mode(cfg.mosi_io, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(cfg.miso_io, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(cfg.sclk_io, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(cfg.cs_io, GPIO_PULLUP_ONLY);

    /* SPI bus init */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = cfg.mosi_io,
        .miso_io_num = cfg.miso_io,
        .sclk_io_num = cfg.sclk_io,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    err = spi_bus_initialize(cfg.host_id, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* SD SPI device config */
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = cfg.host_id;
    host.max_freq_khz = cfg.max_freq_khz;

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = cfg.cs_io;
    slot_cfg.gpio_cd = -1;      /* card-detect pin not used */
    slot_cfg.host_id = cfg.host_id;

    /* Mount config */
    esp_vfs_fat_mount_config_t mount_cfg = {
        .max_files = cfg.max_open_files,
        .format_if_mount_failed = false,
        .allocation_unit_size = cfg.allocation_unit_size,
    };

    /* Wait for SD card to stabilize after power-on */
    vTaskDelay(pdMS_TO_TICKS(1000));

    err = esp_vfs_fat_sdspi_mount(cfg.mount_point, &host, &slot_cfg,
                                  &mount_cfg, &s_card);
    if (err != ESP_OK) {
        if (err == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount FAT filesystem. Is the SD card formatted as FAT?");
        } else {
            ESP_LOGE(TAG, "Failed to init SD card: %s", esp_err_to_name(err));
        }
        spi_bus_free(cfg.host_id);
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
