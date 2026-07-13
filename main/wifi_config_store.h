#pragma once

#include "esp_err.h"
#include "wifi_manager.h"

#include <stdbool.h>

/** Load credentials from the application LittleFS JSON file. */
esp_err_t wifi_config_store_load(wifi_manager_credentials_t *credentials);

/** Persist credentials to the application LittleFS JSON file. */
esp_err_t wifi_config_store_save(
    const wifi_manager_credentials_t *credentials);

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
