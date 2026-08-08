#pragma once

#include "esp_err.h"
#include "wifi_manager.h"

#include <stdbool.h>

/** Application-persisted WiFi configuration: STA profile + AP identity. */
typedef struct {
    wifi_manager_credentials_t sta;
    char ap_ssid[WIFI_MANAGER_SSID_MAX_BYTES + 1u];
    char ap_password[WIFI_MANAGER_PASSWORD_MAX_BYTES + 1u];
} wifi_persisted_config_t;

/** Load the STA profile from the application LittleFS JSON file. */
esp_err_t wifi_config_store_load(wifi_manager_credentials_t *credentials);

/** Load the full persisted config (STA profile + AP identity). */
esp_err_t wifi_config_store_load_full(wifi_persisted_config_t *config);

/** Persist credentials to the application LittleFS JSON file. */
esp_err_t wifi_config_store_save(
    const wifi_manager_credentials_t *credentials);

/**
 * Apply new credentials as a single transaction: stage → snapshot previous →
 * apply to the WiFi manager → commit, rolling back (or entering provisioning
 * mode) on commit failure. Persists on success, reverts on failure.
 */
esp_err_t wifi_config_store_apply_credentials(
    const wifi_manager_credentials_t *credentials);

/**
 * Apply the full config (STA + AP identity) as a single transaction:
 * stage → apply STA → apply AP → commit, with rollback on failure.
 */
esp_err_t wifi_config_store_apply_full(wifi_persisted_config_t *config);

/** Write and fsync credentials to the protected temporary path. */
esp_err_t wifi_config_store_stage(
    const wifi_manager_credentials_t *credentials);

/** Atomically replace the live file with a previously staged file. */
esp_err_t wifi_config_store_commit(void);

/** Remove a staged file; missing staged data is treated as success. */
esp_err_t wifi_config_store_discard(void);

/** True when the credential JSON file currently exists. */
bool wifi_config_store_exists(void);

/** True for application-owned credential paths that must not be exposed. */
bool wifi_config_store_is_path(const char *path);

/** Absolute VFS path of the credential JSON file. */
const char *wifi_config_store_get_path(void);
