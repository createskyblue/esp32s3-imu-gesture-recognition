#pragma once

#include "esp_err.h"

/** Default log directory used by sd_logger_init(). */
#define SD_LOGGER_DEFAULT_LOG_DIR "/sdcard/log"

/**
 * Caller-owned log directory copied by sd_logger_init_with_config().
 * The mount point is intentionally supplied by the application so that
 * sd_logger never hardcodes the SD card mount path itself.
 */
typedef struct {
    const char *log_dir;   /* default: "/sdcard/log" */
} sd_logger_config_t;

/**
 * Initialize SD card logger using the default log directory: create log
 * directory, open first log file, redirect all ESP log output to SD card
 * in addition to UART.
 */
esp_err_t sd_logger_init(void);

/** Initialize SD card logger using caller-provided log directory. */
esp_err_t sd_logger_init_with_config(const sd_logger_config_t *config);

/**
 * Notify the logger that SNTP time has been synchronized.
 * Triggers an immediate file rotation to use real timestamps in the filename.
 */
void sd_logger_notify_time_synced(void);

/**
 * Get the path of the log file currently being written.
 * Returns empty string if no file is open.
 * The returned pointer is valid until the next file rotation.
 */
const char *sd_logger_get_current_path(void);
