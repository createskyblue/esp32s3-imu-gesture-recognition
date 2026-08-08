#include "wifi_config_store.h"

#include "app_storage.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "WIFI_CFG";

#define WIFI_CONFIG_STORE_PATH APP_LITTLEFS_BASE_PATH "/wifi_config.json"
#define WIFI_CONFIG_TEMP_PATH  WIFI_CONFIG_STORE_PATH ".tmp"
#define WIFI_CONFIG_JSON_BUFFER_BYTES 1536u

static bool json_string(cJSON *root, const char *key,
                        char *dest, size_t dest_size, bool required)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (item == NULL) return !required;
    if (!cJSON_IsString(item) || item->valuestring == NULL) return false;
    const size_t length = strlen(item->valuestring);
    if (length >= dest_size) return false;
    memcpy(dest, item->valuestring, length + 1u);
    return true;
}

static void copy_str(char *dest, size_t dest_size, const char *src)
{
    if (dest == NULL || dest_size == 0u) return;
    if (src == NULL) { dest[0] = '\0'; return; }
    snprintf(dest, dest_size, "%s", src);
}

/* "ip_mode" maps to the ip_static flag; missing defaults to DHCP. */
static bool json_ip_mode(cJSON *root, bool *ip_static)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "ip_mode");
    if (item == NULL) { *ip_static = false; return true; }
    if (!cJSON_IsString(item) || item->valuestring == NULL) return false;
    if (strcmp(item->valuestring, "static") == 0) { *ip_static = true; return true; }
    if (strcmp(item->valuestring, "dhcp") == 0) { *ip_static = false; return true; }
    return false;
}

static bool credentials_are_valid(
    const wifi_manager_credentials_t *credentials)
{
    if (credentials == NULL) return false;
    const size_t ssid_len = strnlen(credentials->sta_ssid,
                                    sizeof(credentials->sta_ssid));
    const size_t password_len = strnlen(credentials->sta_password,
                                         sizeof(credentials->sta_password));
    if (ssid_len == 0u || ssid_len > WIFI_MANAGER_SSID_MAX_BYTES ||
        password_len > WIFI_MANAGER_PASSWORD_MAX_BYTES) {
        return false;
    }
    if (!credentials->ip_static) return true;
    return wifi_manager_ipv4_is_valid(credentials->ip_addr) &&
           wifi_manager_ipv4_is_valid(credentials->ip_netmask) &&
           wifi_manager_ipv4_is_valid(credentials->ip_gateway) &&
           wifi_manager_ipv4_is_valid(credentials->ip_dns);
}

static bool ap_identity_is_valid(const wifi_persisted_config_t *config)
{
    if (config == NULL) return false;
    const size_t ssid_len = strnlen(config->ap_ssid, sizeof(config->ap_ssid));
    const size_t password_len = strnlen(config->ap_password,
                                        sizeof(config->ap_password));
    return ssid_len <= WIFI_MANAGER_SSID_MAX_BYTES &&
           password_len <= WIFI_MANAGER_PASSWORD_MAX_BYTES &&
           (password_len == 0u || password_len >= 8u);
}

static bool config_is_valid(const wifi_persisted_config_t *config)
{
    return credentials_are_valid(&config->sta) && ap_identity_is_valid(config);
}

/* Field-wise equality; struct memcmp would trip over padding bytes. */
static bool sta_equal(const wifi_manager_credentials_t *a,
                      const wifi_manager_credentials_t *b)
{
    return a->ip_static == b->ip_static &&
           strcmp(a->sta_ssid, b->sta_ssid) == 0 &&
           strcmp(a->sta_password, b->sta_password) == 0 &&
           strcmp(a->ip_addr, b->ip_addr) == 0 &&
           strcmp(a->ip_netmask, b->ip_netmask) == 0 &&
           strcmp(a->ip_gateway, b->ip_gateway) == 0 &&
           strcmp(a->ip_dns, b->ip_dns) == 0;
}

static esp_err_t load_unlocked(wifi_persisted_config_t *config)
{
    *config = (wifi_persisted_config_t){0};

    FILE *file = fopen(WIFI_CONFIG_STORE_PATH, "r");
    if (file == NULL) {
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    /* Heap buffer: the JSON can be ~1.5 KB and the store runs on the small
     * app_main stack, so a stack buffer would risk overflowing it. */
    char *json = malloc(WIFI_CONFIG_JSON_BUFFER_BYTES);
    if (json == NULL) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    const size_t bytes_read = fread(json, 1u,
                                    WIFI_CONFIG_JSON_BUFFER_BYTES - 1u, file);
    const bool complete = feof(file) != 0;
    fclose(file);
    if (!complete) {
        free(json);
        return ESP_ERR_INVALID_SIZE;
    }
    json[bytes_read] = '\0';

    cJSON *root = cJSON_Parse(json);
    free(json);
    if (root == NULL) return ESP_ERR_INVALID_RESPONSE;

    wifi_persisted_config_t parsed = {0};
    const bool valid = json_string(root, "ssid", parsed.sta.sta_ssid,
                                   sizeof(parsed.sta.sta_ssid), true) &&
                       json_string(root, "password", parsed.sta.sta_password,
                                   sizeof(parsed.sta.sta_password), false) &&
                       json_ip_mode(root, &parsed.sta.ip_static) &&
                       json_string(root, "static_ip", parsed.sta.ip_addr,
                                   sizeof(parsed.sta.ip_addr), false) &&
                       json_string(root, "netmask", parsed.sta.ip_netmask,
                                   sizeof(parsed.sta.ip_netmask), false) &&
                       json_string(root, "gateway", parsed.sta.ip_gateway,
                                   sizeof(parsed.sta.ip_gateway), false) &&
                       json_string(root, "dns", parsed.sta.ip_dns,
                                   sizeof(parsed.sta.ip_dns), false) &&
                       json_string(root, "ap_ssid", parsed.ap_ssid,
                                   sizeof(parsed.ap_ssid), false) &&
                       json_string(root, "ap_password", parsed.ap_password,
                                   sizeof(parsed.ap_password), false) &&
                       config_is_valid(&parsed);
    cJSON_Delete(root);
    if (!valid) return ESP_ERR_INVALID_RESPONSE;

    *config = parsed;
    return ESP_OK;
}

esp_err_t wifi_config_store_load(wifi_manager_credentials_t *credentials)
{
    if (credentials == NULL) return ESP_ERR_INVALID_ARG;
    *credentials = (wifi_manager_credentials_t){0};

    wifi_persisted_config_t config = {0};
    esp_err_t err = app_storage_acquire();
    if (err != ESP_OK) return err;
    err = load_unlocked(&config);
    app_storage_release();
    if (err == ESP_OK) *credentials = config.sta;
    return err;
}

esp_err_t wifi_config_store_load_full(wifi_persisted_config_t *config)
{
    if (config == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t err = app_storage_acquire();
    if (err != ESP_OK) return err;
    err = load_unlocked(config);
    app_storage_release();
    return err;
}

static esp_err_t stage_unlocked(const wifi_persisted_config_t *config)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return ESP_ERR_NO_MEM;
    const bool json_fields_added =
        cJSON_AddStringToObject(root, "ssid", config->sta.sta_ssid) != NULL &&
        cJSON_AddStringToObject(root, "password",
                                config->sta.sta_password) != NULL &&
        cJSON_AddStringToObject(root, "ip_mode",
                                config->sta.ip_static ? "static" : "dhcp") != NULL &&
        cJSON_AddStringToObject(root, "static_ip", config->sta.ip_addr) != NULL &&
        cJSON_AddStringToObject(root, "netmask", config->sta.ip_netmask) != NULL &&
        cJSON_AddStringToObject(root, "gateway", config->sta.ip_gateway) != NULL &&
        cJSON_AddStringToObject(root, "dns", config->sta.ip_dns) != NULL &&
        cJSON_AddStringToObject(root, "ap_ssid", config->ap_ssid) != NULL &&
        cJSON_AddStringToObject(root, "ap_password", config->ap_password) != NULL;
    if (!json_fields_added) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) return ESP_ERR_NO_MEM;

    FILE *file = fopen(WIFI_CONFIG_TEMP_PATH, "w");
    if (file == NULL) {
        cJSON_free(json);
        return ESP_FAIL;
    }

    const size_t json_length = strlen(json);
    bool write_ok = fwrite(json, 1u, json_length, file) == json_length;
    if (write_ok) write_ok = fflush(file) == 0;
    if (write_ok) write_ok = fsync(fileno(file)) == 0;
    if (fclose(file) != 0) write_ok = false;
    cJSON_free(json);

    if (!write_ok) {
        (void)unlink(WIFI_CONFIG_TEMP_PATH);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t commit_unlocked(void)
{
    return rename(WIFI_CONFIG_TEMP_PATH, WIFI_CONFIG_STORE_PATH) == 0
        ? ESP_OK : ESP_FAIL;
}

static esp_err_t discard_unlocked(void)
{
    if (unlink(WIFI_CONFIG_TEMP_PATH) == 0 || errno == ENOENT) return ESP_OK;
    return ESP_FAIL;
}

/* Build a full config from STA-only input, preserving any saved AP identity. */
static esp_err_t build_full_with_sta(wifi_persisted_config_t *config,
                                     const wifi_manager_credentials_t *credentials)
{
    if (config == NULL || credentials == NULL) return ESP_ERR_INVALID_ARG;
    *config = (wifi_persisted_config_t){0};
    esp_err_t err = app_storage_acquire();
    if (err == ESP_OK) {
        (void)load_unlocked(config); /* best-effort: keeps saved AP identity */
        app_storage_release();
    }
    config->sta = *credentials;
    return ESP_OK;
}

esp_err_t wifi_config_store_stage(
    const wifi_manager_credentials_t *credentials)
{
    if (!credentials_are_valid(credentials)) return ESP_ERR_INVALID_ARG;

    wifi_persisted_config_t config;
    esp_err_t err = build_full_with_sta(&config, credentials);
    if (err != ESP_OK) return err;

    err = app_storage_acquire();
    if (err != ESP_OK) return err;
    err = stage_unlocked(&config);
    app_storage_release();
    return err;
}

esp_err_t wifi_config_store_commit(void)
{
    esp_err_t err = app_storage_acquire();
    if (err != ESP_OK) return err;
    err = commit_unlocked();
    app_storage_release();
    return err;
}

esp_err_t wifi_config_store_discard(void)
{
    esp_err_t err = app_storage_acquire();
    if (err != ESP_OK) return err;
    err = discard_unlocked();
    app_storage_release();
    return err;
}

esp_err_t wifi_config_store_save(
    const wifi_manager_credentials_t *credentials)
{
    if (!credentials_are_valid(credentials)) return ESP_ERR_INVALID_ARG;

    wifi_persisted_config_t config;
    esp_err_t err = build_full_with_sta(&config, credentials);
    if (err != ESP_OK) return err;

    err = app_storage_acquire();
    if (err != ESP_OK) return err;
    err = stage_unlocked(&config);
    if (err == ESP_OK) err = commit_unlocked();
    if (err != ESP_OK) (void)discard_unlocked();
    app_storage_release();
    return err;
}

bool wifi_config_store_exists(void)
{
    if (app_storage_try_acquire() != ESP_OK) return false;
    struct stat st;
    const bool exists =
        stat(WIFI_CONFIG_STORE_PATH, &st) == 0 && S_ISREG(st.st_mode);
    app_storage_release();
    return exists;
}

bool wifi_config_store_is_path(const char *path)
{
    if (path == NULL) return false;
    return strcmp(path, WIFI_CONFIG_STORE_PATH) == 0 ||
           strcmp(path, WIFI_CONFIG_TEMP_PATH) == 0;
}

const char *wifi_config_store_get_path(void)
{
    return WIFI_CONFIG_STORE_PATH;
}

esp_err_t wifi_config_store_apply_credentials(
    const wifi_manager_credentials_t *credentials)
{
    if (!credentials_are_valid(credentials)) return ESP_ERR_INVALID_ARG;

    wifi_persisted_config_t config;
    esp_err_t err = build_full_with_sta(&config, credentials);
    if (err != ESP_OK) return err;
    return wifi_config_store_apply_full(&config);
}

esp_err_t wifi_config_store_apply_full(wifi_persisted_config_t *config)
{
    if (!config_is_valid(config)) return ESP_ERR_INVALID_ARG;

    /* No persisted AP identity yet (older config file): keep and persist the
     * current live one rather than clearing the AP. */
    if (config->ap_ssid[0] == '\0') {
        copy_str(config->ap_ssid, sizeof(config->ap_ssid),
                 wifi_manager_get_ap_ssid());
        copy_str(config->ap_password, sizeof(config->ap_password),
                 wifi_manager_get_ap_password());
    }

    esp_err_t err = app_storage_acquire();
    if (err != ESP_OK) return err;

    err = stage_unlocked(config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stage WiFi config failed: %s", esp_err_to_name(err));
        (void)discard_unlocked();
        app_storage_release();
        return err;
    }

    wifi_persisted_config_t previous;
    wifi_manager_get_credentials(&previous.sta);
    copy_str(previous.ap_ssid, sizeof(previous.ap_ssid),
             wifi_manager_get_ap_ssid());
    copy_str(previous.ap_password, sizeof(previous.ap_password),
             wifi_manager_get_ap_password());

    /* Apply only what actually changed, to avoid needless STA reconnects or
     * AP reconfiguration on unrelated saves. */
    const bool sta_changed = !sta_equal(&previous.sta, &config->sta);
    const bool ap_changed = strcmp(previous.ap_ssid, config->ap_ssid) != 0 ||
                            strcmp(previous.ap_password, config->ap_password) != 0;

    if (sta_changed) {
        err = wifi_manager_set_credentials(&config->sta);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "apply WiFi config failed: %s", esp_err_to_name(err));
            (void)discard_unlocked();
            app_storage_release();
            return err;
        }
    }

    if (ap_changed) {
        err = wifi_manager_set_ap_config(config->ap_ssid, config->ap_password);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "apply AP config failed: %s", esp_err_to_name(err));
            (void)discard_unlocked();
            if (sta_changed &&
                wifi_manager_set_credentials(&previous.sta) != ESP_OK) {
                (void)wifi_manager_enter_provisioning_mode();
            }
            app_storage_release();
            return err;
        }
    }

    err = commit_unlocked();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "commit WiFi config failed: %s", esp_err_to_name(err));
        (void)discard_unlocked();
        if (sta_changed) {
            const esp_err_t rollback_err =
                wifi_manager_set_credentials(&previous.sta);
            if (rollback_err != ESP_OK) {
                ESP_LOGE(TAG, "rollback WiFi config failed: %s",
                         esp_err_to_name(rollback_err));
                const esp_err_t safe_mode_err =
                    wifi_manager_enter_provisioning_mode();
                if (safe_mode_err != ESP_OK) {
                    ESP_LOGE(TAG, "force provisioning mode failed: %s",
                             esp_err_to_name(safe_mode_err));
                }
            }
        }
        if (ap_changed) {
            const esp_err_t ap_rollback_err =
                wifi_manager_set_ap_config(previous.ap_ssid,
                                           previous.ap_password);
            if (ap_rollback_err != ESP_OK) {
                ESP_LOGE(TAG, "rollback AP config failed: %s",
                         esp_err_to_name(ap_rollback_err));
            }
        }
        app_storage_release();
        return err;
    }

    app_storage_release();
    return ESP_OK;
}
