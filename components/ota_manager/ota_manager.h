#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_PHASE_IDLE = 0,
    OTA_PHASE_RUNNING,
    OTA_PHASE_DONE,
    OTA_PHASE_FAILED,
} ota_phase_t;

/** Point-in-time snapshot of OTA progress (thread-safe). */
typedef struct {
    ota_phase_t phase;
    char current_label[16];
    char message[128];
    int  progress;
    uint32_t item_index;
    uint32_t item_count;
    uint64_t bytes_written;
    uint64_t total_bytes;
    esp_err_t last_error;
    bool reboot_required;
} ota_status_t;

typedef esp_err_t (*ota_filesystem_update_callback_t)(void *context);

/**
 * Application-provided storage lifecycle used only for filesystem OTA.
 * A successful begin is paired with end from the same OTA task.
 */
typedef struct {
    const char *filesystem_partition_label;
    ota_filesystem_update_callback_t filesystem_update_begin;
    ota_filesystem_update_callback_t filesystem_update_end;
    void *context;
} ota_manager_config_t;

/** Create OTA state for firmware-only updates. */
esp_err_t ota_manager_init(void);

/** Create OTA state with application-provided filesystem update hooks. */
esp_err_t ota_manager_init_with_config(const ota_manager_config_t *config);

/**
 * Register all OTA HTTP handlers on the given server.
 * Registers: /ota/start, /ota/status, /ota/upload/firmware, /ota/upload/filesystem
 */
esp_err_t ota_manager_register(httpd_handle_t server);

/** Snapshot current OTA state. */
void ota_manager_get_status(ota_status_t *out);

/** True while an OTA session is in progress. */
bool ota_manager_is_busy(void);

/**
 * Start a background OTA download + flash for the given URLs.
 * At least one URL must be non-empty.
 */
esp_err_t ota_manager_start_url(const char *firmware_url,
                                const char *filesystem_url);

/* ── Synchronous upload helpers (called by HTTP upload handlers) ───── */

esp_err_t ota_manager_upload_firmware_begin(void);
esp_err_t ota_manager_upload_firmware_write(const uint8_t *data, size_t len);
esp_err_t ota_manager_upload_firmware_end(void);
void      ota_manager_upload_firmware_abort(void);

esp_err_t ota_manager_upload_fs_begin(void);
esp_err_t ota_manager_upload_fs_write(const uint8_t *data, size_t len);
esp_err_t ota_manager_upload_fs_end(void);
void      ota_manager_upload_fs_abort(void);

/** Schedule an esp_restart() after a short delay. */
void ota_manager_restart(void);

#ifdef __cplusplus
}
#endif
