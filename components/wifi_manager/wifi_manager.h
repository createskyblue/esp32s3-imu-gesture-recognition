#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Immutable snapshot of current WiFi state (for HTTP handlers). */
typedef struct {
    bool sta_connected;
    char sta_ssid[33];
    char sta_ip[16];
    char ap_ip[16];
    bool config_loaded;
    bool has_password;
} wifi_snapshot_t;

/** Full WiFi initialisation: APSTA, DNS hijack, SNTP. */
esp_err_t wifi_manager_init(void);

/** Fill a point-in-time snapshot of WiFi state. */
void wifi_manager_get_snapshot(wifi_snapshot_t *out);

/**
 * Save credentials to LittleFS under /littlefs/wifi_config.json,
 * then trigger an immediate STA reconnect with the new SSID/password.
 */
esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password);

/** Constants exposed for JSON responses. */
const char *wifi_manager_get_ap_ssid(void);
const char *wifi_manager_get_config_path(void);

#ifdef __cplusplus
}
#endif
