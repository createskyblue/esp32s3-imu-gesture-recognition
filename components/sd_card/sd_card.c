#include "sd_card.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

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

#define SD_MOSI_GPIO GPIO_NUM_11
#define SD_SCLK_GPIO GPIO_NUM_12
#define SD_MISO_GPIO GPIO_NUM_13
#define SD_CS_GPIO   GPIO_NUM_10
#define SD_CD_GPIO   GPIO_NUM_14

#define SD_MOUNT_POINT "/sdcard"

static sdmmc_card_t *s_card = NULL;

#define MAX_PENDING_DIRS 32

static void list_directory(const char *root_path)
{
    /* Iterative BFS traversal — no recursion, stack-safe */
    static char pending[MAX_PENDING_DIRS][512];
    int head = 0;  /* next to process */
    int tail = 0;  /* next free slot */

    snprintf(pending[0], sizeof(pending[0]), "%s", root_path);
    tail = 1;

    while (head < tail) {
        char dir_path[512];
        snprintf(dir_path, sizeof(dir_path), "%s", pending[head]);
        head++;

        DIR *dir = opendir(dir_path);
        if (dir == NULL) {
            ESP_LOGE(TAG, "failed to open directory: %s", dir_path);
            continue;
        }

        ESP_LOGI(TAG, "=== Directory: %s ===", dir_path);
        int count = 0;
        struct dirent *entry;

        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            char full_path[512];
            const int len = snprintf(full_path, sizeof(full_path),
                                     "%s/%s", dir_path, entry->d_name);
            if (len < 0 || (size_t)len >= sizeof(full_path)) {
                ESP_LOGW(TAG, "  %-36s  (path too long)", entry->d_name);
                count++;
                continue;
            }

            struct stat st;
            if (stat(full_path, &st) == 0) {
                if (entry->d_type == DT_DIR) {
                    ESP_LOGI(TAG, "  %-36s  [DIR]", entry->d_name);
                    /* Enqueue subdirectory */
                    if (tail < MAX_PENDING_DIRS) {
                        snprintf(pending[tail], sizeof(pending[tail]), "%s", full_path);
                        tail++;
                    }
                } else {
                    ESP_LOGI(TAG, "  %-36s %8ld bytes", entry->d_name, (long)st.st_size);
                }
            } else {
                ESP_LOGI(TAG, "  %-36s  (stat failed)", entry->d_name);
            }
            count++;
        }
        ESP_LOGI(TAG, "=== %d entries ===", count);
        closedir(dir);
    }

    ESP_LOGI(TAG, "=== %d directories scanned ===", tail);
}

static void read_write_test(void)
{
    const char *test_path = SD_MOUNT_POINT "/test.txt";
    const char *test_data = "ESP32-S3 SD card read/write test OK!";
    const size_t test_len = strlen(test_data);

    /* write */
    ESP_LOGI(TAG, "Writing to %s ...", test_path);
    FILE *f = fopen(test_path, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "failed to open file for writing");
        return;
    }
    fprintf(f, "%s", test_data);
    fclose(f);
    ESP_LOGI(TAG, "Wrote %u bytes", (unsigned)test_len);

    /* read back */
    ESP_LOGI(TAG, "Reading back from %s ...", test_path);
    f = fopen(test_path, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "failed to open file for reading");
        return;
    }
    char buf[128] = {0};
    size_t nread = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    ESP_LOGI(TAG, "Read %u bytes: \"%s\"", (unsigned)nread, buf);

    /* verify */
    if (nread == test_len && memcmp(buf, test_data, test_len) == 0) {
        ESP_LOGI(TAG, "Read/write test PASSED");
    } else {
        ESP_LOGE(TAG, "Read/write test FAILED (mismatch)");
    }
}

esp_err_t sd_card_init(void)
{
    ESP_LOGI(TAG, "Initializing SD card (SPI mode)...");
    ESP_LOGI(TAG, "  MOSI=IO%d, SCLK=IO%d, MISO=IO%d, CS=IO%d, CD=IO%d",
             SD_MOSI_GPIO, SD_SCLK_GPIO, SD_MISO_GPIO, SD_CS_GPIO, SD_CD_GPIO);

    /* Enable internal pull-ups on SPI pins */
    gpio_set_pull_mode(SD_MOSI_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(SD_MISO_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(SD_SCLK_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(SD_CS_GPIO, GPIO_PULLUP_ONLY);

    /* SPI bus init */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_MOSI_GPIO,
        .miso_io_num = SD_MISO_GPIO,
        .sclk_io_num = SD_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* SD SPI device config */
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = 20000;  /* 20MHz */

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = SD_CS_GPIO;
    slot_cfg.gpio_cd = -1;  /* 暂时不用 CD 引脚 */
    slot_cfg.host_id = SPI2_HOST;

    /* Mount config */
    esp_vfs_fat_mount_config_t mount_cfg = {
        .max_files = 8,
        .format_if_mount_failed = false,
        .allocation_unit_size = 16 * 1024,
    };

    /* Wait for SD card to stabilize after power-on */
    vTaskDelay(pdMS_TO_TICKS(1000));

    err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_cfg, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        if (err == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount FAT filesystem. Is the SD card formatted as FAT?");
        } else {
            ESP_LOGE(TAG, "Failed to init SD card: %s", esp_err_to_name(err));
        }
        spi_bus_free(SPI2_HOST);
        return err;
    }

    /* Print card info */
    sdmmc_card_print_info(stdout, s_card);

    /* List root directory recursively */
    list_directory(SD_MOUNT_POINT);

    /* Read/write test */
    read_write_test();

    ESP_LOGI(TAG, "SD card ready at %s", SD_MOUNT_POINT);
    return ESP_OK;
}
