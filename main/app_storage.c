#include "app_storage.h"

#include <stdbool.h>

#include "esp_littlefs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "APP_STORAGE";
static SemaphoreHandle_t s_access_mutex;
static bool s_mounted;
static bool s_update_active;

static esp_err_t mount_littlefs(void)
{
    const esp_vfs_littlefs_conf_t conf = {
        .base_path = APP_LITTLEFS_BASE_PATH,
        .partition_label = APP_LITTLEFS_PARTITION_LABEL,
        .format_if_mount_failed = false,
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t total = 0u;
    size_t used = 0u;
    err = esp_littlefs_info(conf.partition_label, &total, &used);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LittleFS info failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "LittleFS mounted: total=%u used=%u",
                 (unsigned)total, (unsigned)used);
    }
    return ESP_OK;
}

static esp_err_t acquire_with_timeout(TickType_t timeout)
{
    if (s_access_mutex == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTakeRecursive(s_access_mutex, timeout) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_mounted) {
        (void)xSemaphoreGiveRecursive(s_access_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t app_storage_init(void)
{
    if (s_access_mutex == NULL) {
        s_access_mutex = xSemaphoreCreateRecursiveMutex();
        if (s_access_mutex == NULL) return ESP_ERR_NO_MEM;
    }

    (void)xSemaphoreTakeRecursive(s_access_mutex, portMAX_DELAY);
    if (s_mounted) {
        (void)xSemaphoreGiveRecursive(s_access_mutex);
        return ESP_OK;
    }

    esp_err_t err = mount_littlefs();
    if (err == ESP_OK) s_mounted = true;
    (void)xSemaphoreGiveRecursive(s_access_mutex);
    return err;
}

esp_err_t app_storage_acquire(void)
{
    return acquire_with_timeout(portMAX_DELAY);
}

esp_err_t app_storage_try_acquire(void)
{
    return acquire_with_timeout(0);
}

void app_storage_release(void)
{
    if (s_access_mutex != NULL) {
        (void)xSemaphoreGiveRecursive(s_access_mutex);
    }
}

esp_err_t app_storage_begin_update(void *context)
{
    (void)context;
    if (s_access_mutex == NULL) return ESP_ERR_INVALID_STATE;

    (void)xSemaphoreTakeRecursive(s_access_mutex, portMAX_DELAY);
    if (s_update_active) {
        (void)xSemaphoreGiveRecursive(s_access_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    if (s_mounted) {
        esp_err_t err =
            esp_vfs_littlefs_unregister(APP_LITTLEFS_PARTITION_LABEL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "LittleFS unmount failed: %s", esp_err_to_name(err));
            (void)xSemaphoreGiveRecursive(s_access_mutex);
            return err;
        }
        s_mounted = false;
        ESP_LOGI(TAG, "LittleFS unmounted for exclusive update");
    } else {
        ESP_LOGW(TAG, "LittleFS is unavailable; starting recovery update");
    }

    s_update_active = true;
    return ESP_OK;
}

esp_err_t app_storage_end_update(void *context)
{
    (void)context;
    if (s_access_mutex == NULL || !s_update_active) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = mount_littlefs();
    s_mounted = err == ESP_OK;
    s_update_active = false;
    (void)xSemaphoreGiveRecursive(s_access_mutex);
    return err;
}
