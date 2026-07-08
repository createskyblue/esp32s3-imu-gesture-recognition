#pragma once

#include "esp_err.h"

/**
 * Initialize SD card logger: create log directory, open first log file,
 * redirect all ESP log output to SD card in addition to UART.
 */
esp_err_t sd_logger_init(void);

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
