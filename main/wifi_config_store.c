#include "wifi_config_store.h"

#include "app_storage.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"

#define WIFI_CONFIG_STORE_PATH APP_LITTLEFS_BASE_PATH "/wifi_config.json"
#define WIFI_CONFIG_TEMP_PATH  WIFI_CONFIG_STORE_PATH ".tmp"
#define WIFI_CONFIG_JSON_BUFFER_BYTES 768u

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

static bool credentials_are_valid(
    const wifi_manager_credentials_t *credentials)
{
    if (credentials == NULL) return false;
    const size_t ssid_len = strnlen(credentials->sta_ssid,
                                    sizeof(credentials->sta_ssid));
    const size_t password_len = strnlen(credentials->sta_password,
                                         sizeof(credentials->sta_password));
    return ssid_len > 0u && ssid_len <= WIFI_MANAGER_SSID_MAX_BYTES &&
           password_len <= WIFI_MANAGER_PASSWORD_MAX_BYTES;
}

static esp_err_t load_unlocked(wifi_manager_credentials_t *credentials)
{
    *credentials = (wifi_manager_credentials_t){0};

    FILE *file = fopen(WIFI_CONFIG_STORE_PATH, "r");
    if (file == NULL) {
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    char json[WIFI_CONFIG_JSON_BUFFER_BYTES];
    const size_t bytes_read = fread(json, 1u, sizeof(json) - 1u, file);
    const bool complete = feof(file) != 0;
    fclose(file);
    if (!complete) return ESP_ERR_INVALID_SIZE;
    json[bytes_read] = '\0';

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) return ESP_ERR_INVALID_RESPONSE;

    wifi_manager_credentials_t parsed = {0};
    const bool valid = json_string(root, "ssid", parsed.sta_ssid,
                                   sizeof(parsed.sta_ssid), true) &&
                       json_string(root, "password", parsed.sta_password,
                                   sizeof(parsed.sta_password), false) &&
                       credentials_are_valid(&parsed);
    cJSON_Delete(root);
    if (!valid) return ESP_ERR_INVALID_RESPONSE;

    *credentials = parsed;
    return ESP_OK;
}

esp_err_t wifi_config_store_load(wifi_manager_credentials_t *credentials)
{
    if (credentials == NULL) return ESP_ERR_INVALID_ARG;
    *credentials = (wifi_manager_credentials_t){0};

    esp_err_t err = app_storage_acquire();
    if (err != ESP_OK) return err;
    err = load_unlocked(credentials);
    app_storage_release();
    return err;
}

static esp_err_t stage_unlocked(
    const wifi_manager_credentials_t *credentials)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return ESP_ERR_NO_MEM;
    const bool json_fields_added =
        cJSON_AddStringToObject(root, "ssid", credentials->sta_ssid) != NULL &&
        cJSON_AddStringToObject(root, "password",
                                credentials->sta_password) != NULL;
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

esp_err_t wifi_config_store_stage(
    const wifi_manager_credentials_t *credentials)
{
    if (!credentials_are_valid(credentials)) return ESP_ERR_INVALID_ARG;

    esp_err_t err = app_storage_acquire();
    if (err != ESP_OK) return err;
    err = stage_unlocked(credentials);
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

    esp_err_t err = app_storage_acquire();
    if (err != ESP_OK) return err;
    err = stage_unlocked(credentials);
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
