#pragma once

#include "esp_err.h"

#define APP_LITTLEFS_BASE_PATH       "/littlefs"
#define APP_LITTLEFS_PARTITION_LABEL "storage"

/** Mount the application LittleFS partition. Call before storage consumers. */
esp_err_t app_storage_init(void);

/** Acquire shared application access to the mounted filesystem. */
esp_err_t app_storage_acquire(void);

/** Try to acquire filesystem access without waiting. */
esp_err_t app_storage_try_acquire(void);

/** Release one acquire/try-acquire operation from the current task. */
void app_storage_release(void);

/**
 * OTA callback: exclusively acquire storage and unmount LittleFS.
 * The successful caller must later call app_storage_end_update() from the
 * same task.
 */
esp_err_t app_storage_begin_update(void *context);

/** OTA callback: remount LittleFS and release the exclusive update lease. */
esp_err_t app_storage_end_update(void *context);
