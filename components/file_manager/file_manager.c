#include "file_manager.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"

static const char *TAG = "FILE_MGR";

#define FILE_MGR_CHUNK_SIZE  1024u
#define FILE_MGR_MAX_PATH    512u
#define FILE_MGR_JSON_BUF    4096u
#define FILE_MGR_UPLOAD_BUF_SIZE 65536u
#define FILE_MGR_UPLOAD_META_MAX 768u
#define FILE_MGR_UPLOAD_TMP_SUFFIX ".upload.tmp"
#define FILE_MGR_MOUNT_POINT_MAX 64u
#define FILE_MGR_PARTITION_LABEL_MAX 16u

static struct {
    char internal_mount_point[FILE_MGR_MOUNT_POINT_MAX];
    char internal_partition_label[FILE_MGR_PARTITION_LABEL_MAX + 1u];
    char sd_mount_point[FILE_MGR_MOUNT_POINT_MAX];
    bool configured;
} s_storage;

static file_manager_mutation_guard_t s_mutation_guard = NULL;
static file_manager_read_guard_t s_read_guard = NULL;
static file_manager_access_begin_t s_access_begin = NULL;
static file_manager_access_end_t s_access_end = NULL;

static bool valid_mount_point(const char *path)
{
    if (path == NULL || path[0] != '/') return false;
    const size_t length = strnlen(path, FILE_MGR_MOUNT_POINT_MAX);
    return length > 1u && length < FILE_MGR_MOUNT_POINT_MAX &&
           path[length - 1u] != '/';
}

esp_err_t file_manager_set_storage_config(
    const file_manager_storage_config_t *config)
{
    if (config == NULL ||
        !valid_mount_point(config->internal_mount_point) ||
        config->internal_partition_label == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t label_length = strnlen(
        config->internal_partition_label,
        FILE_MGR_PARTITION_LABEL_MAX + 1u);
    if (label_length == 0u || label_length > FILE_MGR_PARTITION_LABEL_MAX ||
        (config->sd_mount_point != NULL &&
         !valid_mount_point(config->sd_mount_point))) {
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(s_storage.internal_mount_point,
             sizeof(s_storage.internal_mount_point), "%s",
             config->internal_mount_point);
    memcpy(s_storage.internal_partition_label,
           config->internal_partition_label, label_length + 1u);
    if (config->sd_mount_point != NULL) {
        snprintf(s_storage.sd_mount_point,
                 sizeof(s_storage.sd_mount_point), "%s",
                 config->sd_mount_point);
    } else {
        s_storage.sd_mount_point[0] = '\0';
    }
    s_storage.configured = true;
    return ESP_OK;
}

void file_manager_set_access_callbacks(file_manager_access_begin_t begin,
                                       file_manager_access_end_t end)
{
    s_access_begin = begin;
    s_access_end = end;
}

void file_manager_set_read_guard(file_manager_read_guard_t guard)
{
    s_read_guard = guard;
}

void file_manager_set_mutation_guard(file_manager_mutation_guard_t guard)
{
    s_mutation_guard = guard;
}

/* ── path validation ────────────────────────────────────────────────── */

static bool is_safe_absolute_path(const char *user_path)
{
    if (user_path == NULL || user_path[0] != '/') {
        return false;
    }
    if (strcmp(user_path, "/") == 0) {
        return true;
    }

    const char *segment = user_path + 1;
    while (*segment != '\0') {
        const char *separator = strchr(segment, '/');
        size_t segment_len = separator != NULL
            ? (size_t)(separator - segment) : strlen(segment);
        if (segment_len == 0u ||
            (segment_len == 1u && segment[0] == '.') ||
            (segment_len == 2u && segment[0] == '.' && segment[1] == '.') ||
            memchr(segment, '\\', segment_len) != NULL) {
            return false;
        }
        if (separator == NULL) {
            return true;
        }
        segment = separator + 1;
    }
    return false;
}

static esp_err_t validate_and_resolve_path(
    const char *fs_type,
    const char *user_path,
    char *resolved,
    size_t resolved_size,
    const char **mount_point_out)
{
    const char *mount_point = NULL;

    if (!s_storage.configured || fs_type == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (strcmp(fs_type, "internal") == 0) {
        mount_point = s_storage.internal_mount_point;
    } else if (strcmp(fs_type, "sd") == 0 &&
               s_storage.sd_mount_point[0] != '\0') {
        mount_point = s_storage.sd_mount_point;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    if (!is_safe_absolute_path(user_path)) {
        ESP_LOGW(TAG, "invalid absolute path rejected: %s",
                 user_path != NULL ? user_path : "(null)");
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(user_path, "/") == 0) {
        snprintf(resolved, resolved_size, "%s", mount_point);
        if (mount_point_out) *mount_point_out = mount_point;
        return ESP_OK;
    }

    int len = snprintf(resolved, resolved_size, "%s%s", mount_point, user_path);

    if (len < 0 || (size_t)len >= resolved_size) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strncmp(resolved, mount_point, strlen(mount_point)) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (mount_point_out) *mount_point_out = mount_point;
    return ESP_OK;
}

/* ── filesystem info ───────────────────────────────────────────────── */

static esp_err_t get_filesystem_info(
    const char *fs_type,
    uint64_t *total_bytes,
    uint64_t *free_bytes,
    bool *mounted)
{
    *mounted = false;
    *total_bytes = 0;
    *free_bytes = 0;

    if (strcmp(fs_type, "internal") == 0) {
        size_t total = 0, used = 0;
        esp_err_t err = esp_littlefs_info(
            s_storage.internal_partition_label, &total, &used);
        if (err != ESP_OK) return err;
        *total_bytes = total;
        *free_bytes = (total > used) ? (total - used) : 0;
        *mounted = true;
        return ESP_OK;
    }

    if (strcmp(fs_type, "sd") == 0) {
        struct stat st;
        if (s_storage.sd_mount_point[0] == '\0' ||
            stat(s_storage.sd_mount_point, &st) != 0) {
            *mounted = false;
            return ESP_ERR_NOT_FOUND;
        }
        uint64_t total = 0, free_space = 0;
        esp_err_t err = esp_vfs_fat_info(
            s_storage.sd_mount_point, &total, &free_space);
        if (err != ESP_OK) return err;
        *total_bytes = total;
        *free_bytes = free_space;
        *mounted = true;
        return ESP_OK;
    }

    return ESP_ERR_INVALID_ARG;
}

/* ── compact request/response helpers ──────────────────────────────── */

static int recv_retry_timeout(httpd_req_t *req, char *buffer, size_t length)
{
    int received;
    do {
        received = httpd_req_recv(req, buffer, length);
    } while (received == HTTPD_SOCK_ERR_TIMEOUT);
    return received;
}

static cJSON *receive_json_request(httpd_req_t *req)
{
    char body[FILE_MGR_JSON_BUF];
    if (req->content_len >= sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json body too large");
        return NULL;
    }

    size_t received = 0;
    while (received < req->content_len) {
        int chunk = recv_retry_timeout(req, body + received,
                                       req->content_len - received);
        if (chunk <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body receive failed");
            return NULL;
        }
        received += (size_t)chunk;
    }
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
    }
    return root;
}

static esp_err_t get_required_string(const cJSON *root, const char *field,
                                     char *value, size_t value_size)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, field);
    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        item->valuestring[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    int written = snprintf(value, value_size, "%s", item->valuestring);
    return written >= 0 && (size_t)written < value_size
        ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t get_fs_path(const cJSON *root,
                             char *fs_type, size_t fs_type_size,
                             char *raw_path, size_t raw_path_size)
{
    if (get_required_string(root, "fs", fs_type, fs_type_size) != ESP_OK ||
        get_required_string(root, "path", raw_path, raw_path_size) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t send_json_response(httpd_req_t *req, cJSON *root)
{
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "json allocation failed");
        return ESP_FAIL;
    }

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "json allocation failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    esp_err_t err = httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
    cJSON_free(text);
    return err;
}

static const char *mutation_denied(const char *fs_type, const char *resolved)
{
    return s_mutation_guard != NULL ? s_mutation_guard(fs_type, resolved) : NULL;
}

static const char *read_denied(const char *fs_type, const char *resolved)
{
    return s_read_guard != NULL ? s_read_guard(fs_type, resolved) : NULL;
}

static esp_err_t send_forbidden(httpd_req_t *req, const char *reason);

static esp_err_t begin_file_access(httpd_req_t *req)
{
    if (s_access_begin == NULL) return ESP_OK;
    const esp_err_t err = s_access_begin();
    if (err == ESP_OK) return ESP_OK;

    httpd_resp_set_status(req, "503 Service Unavailable");
    (void)httpd_resp_sendstr(req, "filesystem is temporarily unavailable");
    return err;
}

static void end_file_access(void)
{
    if (s_access_end != NULL) s_access_end();
}

/* ── handler: serve files.html ─────────────────────────────────────── */

static esp_err_t file_manager_page_handler_unlocked(httpd_req_t *req)
{
    char page_path[FILE_MGR_MOUNT_POINT_MAX + sizeof("/files.html")];
    const int path_length = snprintf(page_path, sizeof(page_path),
                                     "%s/files.html",
                                     s_storage.internal_mount_point);
    if (path_length < 0 || (size_t)path_length >= sizeof(page_path)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "files.html path is invalid");
        return ESP_FAIL;
    }
    FILE *file = fopen(page_path, "r");
    if (file == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "files.html not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");

    char buffer[FILE_MGR_CHUNK_SIZE];
    size_t read_bytes;
    esp_err_t err = ESP_OK;
    while ((read_bytes = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        err = httpd_resp_send_chunk(req, buffer, read_bytes);
        if (err != ESP_OK) break;
    }
    fclose(file);

    if (err == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
}

static esp_err_t file_manager_page_handler(httpd_req_t *req)
{
    if (begin_file_access(req) != ESP_OK) return ESP_OK;
    const esp_err_t err = file_manager_page_handler_unlocked(req);
    end_file_access();
    return err;
}

/* ── handler: directory listing + disk space (POST, streaming) ─────── */

static esp_err_t send_str_chunk(httpd_req_t *req, const char *str)
{
    return httpd_resp_send_chunk(req, str, strlen(str));
}

static esp_err_t file_manager_list_action(httpd_req_t *req, const cJSON *root)
{
    char fs_type[16] = {0};
    char raw_path[FILE_MGR_MAX_PATH] = {0};

    if (get_fs_path(root, fs_type, sizeof(fs_type),
                    raw_path, sizeof(raw_path)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "expected fs and path strings");
        return ESP_FAIL;
    }

    char resolved[FILE_MGR_MAX_PATH];
    if (validate_and_resolve_path(fs_type, raw_path, resolved, sizeof(resolved),
                                  NULL) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid path");
        return ESP_FAIL;
    }

    const char *denied = read_denied(fs_type, resolved);
    if (denied != NULL) {
        return send_forbidden(req, denied);
    }

    uint64_t total_bytes = 0, free_bytes = 0;
    bool mounted = false;
    get_filesystem_info(fs_type, &total_bytes, &free_bytes, &mounted);

    DIR *dir = NULL;
    if (mounted || strcmp(fs_type, "sd") != 0) {
        dir = opendir(resolved);
        if (dir == NULL) {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "directory not found");
            return ESP_FAIL;
        }
    }

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");

    cJSON *metadata = cJSON_CreateObject();
    if (metadata == NULL ||
        cJSON_AddStringToObject(metadata, "fs", fs_type) == NULL ||
        cJSON_AddStringToObject(metadata, "current_path",
                                raw_path[0] ? raw_path : "/") == NULL ||
        cJSON_AddNumberToObject(metadata, "total_bytes",
                                (double)total_bytes) == NULL ||
        cJSON_AddNumberToObject(metadata, "free_bytes",
                                (double)free_bytes) == NULL ||
        cJSON_AddBoolToObject(metadata, "mounted", mounted) == NULL) {
        cJSON_Delete(metadata);
        if (dir != NULL) {
            closedir(dir);
        }
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "json allocation failed");
        return ESP_FAIL;
    }

    char *metadata_json = cJSON_PrintUnformatted(metadata);
    cJSON_Delete(metadata);
    if (metadata_json == NULL) {
        if (dir != NULL) {
            closedir(dir);
        }
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "json allocation failed");
        return ESP_FAIL;
    }

    size_t metadata_len = strlen(metadata_json);
    if (metadata_len == 0u || metadata_json[metadata_len - 1u] != '}') {
        cJSON_free(metadata_json);
        if (dir != NULL) {
            closedir(dir);
        }
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "json encoding failed");
        return ESP_FAIL;
    }
    metadata_json[metadata_len - 1u] = '\0';
    esp_err_t header_err = send_str_chunk(req, metadata_json);
    cJSON_free(metadata_json);
    if (header_err == ESP_OK) {
        header_err = send_str_chunk(req, ",\"entries\":[");
    }
    if (header_err != ESP_OK) {
        if (dir != NULL) {
            closedir(dir);
        }
        return ESP_FAIL;
    }

    bool first_entry = true;
    if (dir != NULL) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            char full_path[FILE_MGR_MAX_PATH + 256];
            snprintf(full_path, sizeof(full_path), "%s/%s", resolved, entry->d_name);

            if (read_denied(fs_type, full_path) != NULL) {
                continue;
            }

            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "name", entry->d_name);

            struct stat st;
            if (stat(full_path, &st) == 0) {
                cJSON_AddBoolToObject(item, "is_dir", S_ISDIR(st.st_mode));
                cJSON_AddNumberToObject(item, "size",
                    S_ISDIR(st.st_mode) ? 0 : (double)st.st_size);
                cJSON_AddNumberToObject(item, "modified", (double)st.st_mtime);
            } else {
                cJSON_AddBoolToObject(item, "is_dir",
                    entry->d_type == DT_DIR);
                cJSON_AddNumberToObject(item, "size", 0);
                cJSON_AddNumberToObject(item, "modified", 0);
            }

            char *item_json = cJSON_PrintUnformatted(item);
            cJSON_Delete(item);

            if (item_json != NULL) {
                esp_err_t chunk_err = ESP_OK;
                if (!first_entry) {
                    chunk_err = send_str_chunk(req, ",");
                }
                if (chunk_err == ESP_OK) {
                    chunk_err = send_str_chunk(req, item_json);
                }
                cJSON_free(item_json);
                first_entry = false;
                if (chunk_err != ESP_OK) {
                    ESP_LOGW(TAG, "list stream interrupted");
                    closedir(dir);
                    return ESP_FAIL;
                }
            }
        }
        closedir(dir);
    }

    return send_str_chunk(req, "]}") == ESP_OK
        ? httpd_resp_send_chunk(req, NULL, 0)
        : ESP_FAIL;
}

/* ── handler: file download (POST, chunked binary) ─────────────────── */

static void safe_download_filename(const char *filename,
                                   char *safe_filename,
                                   size_t safe_filename_size)
{
    size_t output = 0;
    for (size_t i = 0; filename[i] != '\0' &&
         output + 1u < safe_filename_size; i++) {
        char ch = filename[i];
        safe_filename[output++] =
            (ch == '"' || ch == '\\' || ch == '\r' || ch == '\n') ? '_' : ch;
    }
    safe_filename[output] = '\0';
}

static esp_err_t file_manager_download_action(httpd_req_t *req,
                                              const cJSON *root)
{
    char fs_type[16] = {0};
    char raw_path[FILE_MGR_MAX_PATH] = {0};

    if (get_fs_path(root, fs_type, sizeof(fs_type),
                    raw_path, sizeof(raw_path)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "expected fs and path strings");
        return ESP_FAIL;
    }

    char resolved[FILE_MGR_MAX_PATH];
    if (validate_and_resolve_path(fs_type, raw_path, resolved, sizeof(resolved),
                                  NULL) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid path");
        return ESP_FAIL;
    }

    const char *denied = read_denied(fs_type, resolved);
    if (denied != NULL) {
        return send_forbidden(req, denied);
    }

    struct stat st;
    if (stat(resolved, &st) != 0 || S_ISDIR(st.st_mode)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "file not found");
        return ESP_FAIL;
    }

    FILE *file = fopen(resolved, "rb");
    if (file == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "cannot open file");
        return ESP_FAIL;
    }

    const char *filename = strrchr(raw_path, '/');
    if (filename == NULL || filename[1] == '\0') {
        filename = raw_path;
    } else {
        filename++;
    }

    httpd_resp_set_type(req, "application/octet-stream");
    char safe_filename[FILE_MGR_MAX_PATH];
    safe_download_filename(filename, safe_filename, sizeof(safe_filename));
    char disposition[560];
    snprintf(disposition, sizeof(disposition),
             "attachment; filename=\"%s\"", safe_filename);
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char *buffer = malloc(FILE_MGR_CHUNK_SIZE);
    if (buffer == NULL) {
        fclose(file);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }

    size_t read_bytes;
    esp_err_t err = ESP_OK;
    while ((read_bytes = fread(buffer, 1, FILE_MGR_CHUNK_SIZE, file)) > 0) {
        err = httpd_resp_send_chunk(req, buffer, read_bytes);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "download interrupted: %s", esp_err_to_name(err));
            break;
        }
    }
    fclose(file);
    free(buffer);

    if (err == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
}

static int hex_to_int(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *str)
{
    char *src = str;
    char *dst = str;
    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            int high = hex_to_int(src[1]);
            int low = hex_to_int(src[2]);
            if (high >= 0 && low >= 0) {
                *dst++ = (char)((high << 4) | low);
                src += 3;
                continue;
            }
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
            continue;
        }
        *dst++ = *src++;
    }
    *dst = '\0';
}

static esp_err_t file_manager_download_get_handler_unlocked(httpd_req_t *req)
{
    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len == 0 || query_len >= FILE_MGR_MAX_PATH * 2) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing query parameters");
        return ESP_FAIL;
    }

    char *query = malloc(query_len + 1);
    if (query == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }

    httpd_req_get_url_query_str(req, query, query_len + 1);

    char fs_type[16] = {0};
    char raw_path[FILE_MGR_MAX_PATH] = {0};
    if (httpd_query_key_value(query, "fs", fs_type, sizeof(fs_type)) != ESP_OK ||
        httpd_query_key_value(query, "path", raw_path, sizeof(raw_path)) != ESP_OK) {
        free(query);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing fs or path query parameter");
        return ESP_FAIL;
    }
    free(query);
    url_decode(fs_type);
    url_decode(raw_path);

    char resolved[FILE_MGR_MAX_PATH];
    if (validate_and_resolve_path(fs_type, raw_path, resolved, sizeof(resolved), NULL) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid path");
        return ESP_FAIL;
    }

    const char *denied = read_denied(fs_type, resolved);
    if (denied != NULL) {
        return send_forbidden(req, denied);
    }

    struct stat st;
    if (stat(resolved, &st) != 0 || S_ISDIR(st.st_mode)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "file not found");
        return ESP_FAIL;
    }

    FILE *file = fopen(resolved, "rb");
    if (file == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "cannot open file");
        return ESP_FAIL;
    }

    const char *filename = strrchr(raw_path, '/');
    if (filename == NULL || filename[1] == '\0') {
        filename = raw_path;
    } else {
        filename++;
    }

    httpd_resp_set_type(req, "application/octet-stream");
    char safe_filename[FILE_MGR_MAX_PATH];
    safe_download_filename(filename, safe_filename, sizeof(safe_filename));
    char disposition[560];
    snprintf(disposition, sizeof(disposition),
             "attachment; filename=\"%s\"", safe_filename);
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char *buffer = heap_caps_malloc(FILE_MGR_UPLOAD_BUF_SIZE,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        buffer = heap_caps_malloc(FILE_MGR_UPLOAD_BUF_SIZE,
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (buffer == NULL) {
        fclose(file);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }

    esp_err_t err = ESP_OK;
    size_t read_bytes;
    while ((read_bytes = fread(buffer, 1, FILE_MGR_UPLOAD_BUF_SIZE, file)) > 0) {
        err = httpd_resp_send_chunk(req, buffer, read_bytes);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "download interrupted: %s", esp_err_to_name(err));
            break;
        }
    }

    fclose(file);
    free(buffer);

    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }

    return err;
}

static esp_err_t file_manager_download_get_handler(httpd_req_t *req)
{
    if (begin_file_access(req) != ESP_OK) return ESP_OK;
    const esp_err_t err = file_manager_download_get_handler_unlocked(req);
    end_file_access();
    return err;
}

/* ── mutation actions ───────────────────────────────────────────────── */

static esp_err_t send_forbidden(httpd_req_t *req, const char *reason)
{
    httpd_resp_set_status(req, "403 Forbidden");
    return httpd_resp_sendstr(req, reason);
}

static esp_err_t file_manager_delete_action(httpd_req_t *req,
                                            const cJSON *root)
{
    char fs_type[16] = {0};
    char raw_path[FILE_MGR_MAX_PATH] = {0};
    if (get_required_string(root, "fs", fs_type, sizeof(fs_type)) != ESP_OK ||
        get_required_string(root, "path", raw_path, sizeof(raw_path)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "expected fs and path strings");
        return ESP_FAIL;
    }
    if (strcmp(raw_path, "/") == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "cannot delete root");
        return ESP_FAIL;
    }

    char resolved[FILE_MGR_MAX_PATH];
    if (validate_and_resolve_path(fs_type, raw_path, resolved,
                                  sizeof(resolved), NULL) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid path");
        return ESP_FAIL;
    }

    const char *denied = mutation_denied(fs_type, resolved);
    if (denied != NULL) {
        return send_forbidden(req, denied);
    }

    struct stat st;
    if (stat(resolved, &st) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_FAIL;
    }

    int ret = S_ISDIR(st.st_mode) ? rmdir(resolved) : unlink(resolved);
    if (ret != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            S_ISDIR(st.st_mode) ? "directory not empty"
                                                : "delete failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "deleted: %s", resolved);
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "deleted", true);
    return send_json_response(req, response);
}

static esp_err_t file_manager_mkdir_action(httpd_req_t *req,
                                           const cJSON *root)
{
    char fs_type[16] = {0};
    char raw_path[FILE_MGR_MAX_PATH] = {0};
    if (get_fs_path(root, fs_type, sizeof(fs_type),
                    raw_path, sizeof(raw_path)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "expected fs and path strings");
        return ESP_FAIL;
    }
    if (strcmp(raw_path, "/") == 0) {
        cJSON *response = cJSON_CreateObject();
        cJSON_AddBoolToObject(response, "created", false);
        return send_json_response(req, response);
    }

    char resolved[FILE_MGR_MAX_PATH];
    if (validate_and_resolve_path(fs_type, raw_path, resolved,
                                  sizeof(resolved), NULL) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid path");
        return ESP_FAIL;
    }

    const char *denied = mutation_denied(fs_type, resolved);
    if (denied != NULL) {
        return send_forbidden(req, denied);
    }

    struct stat st;
    if (stat(resolved, &st) == 0) {
        if (!S_ISDIR(st.st_mode)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "path already exists as file");
            return ESP_FAIL;
        }
        cJSON *response = cJSON_CreateObject();
        cJSON_AddBoolToObject(response, "created", false);
        return send_json_response(req, response);
    }
    if (mkdir(resolved, 0775) != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mkdir failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "directory created: %s", resolved);
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "created", true);
    return send_json_response(req, response);
}

static cJSON *receive_upload_metadata(httpd_req_t *req, size_t *bytes_used)
{
    char metadata[FILE_MGR_UPLOAD_META_MAX];
    size_t used = 0;

    while (used + 1u < sizeof(metadata) && used < req->content_len) {
        int received = recv_retry_timeout(req, &metadata[used], 1u);
        if (received <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "upload metadata receive failed");
            return NULL;
        }
        if (metadata[used++] == '\n') {
            metadata[used - 1u] = '\0';
            *bytes_used = used;
            cJSON *root = cJSON_Parse(metadata);
            if (root == NULL) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                    "invalid upload metadata");
            }
            return root;
        }
    }

    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "upload metadata too large");
    return NULL;
}

static esp_err_t stream_upload_body(httpd_req_t *req, FILE *file,
                                    size_t bytes_remaining)
{
    char *buffer = heap_caps_malloc(FILE_MGR_UPLOAD_BUF_SIZE,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        ESP_LOGW(TAG, "PSRAM upload buffer unavailable; falling back to internal");
        buffer = heap_caps_malloc(FILE_MGR_UPLOAD_BUF_SIZE,
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    while (bytes_remaining > 0u) {
        size_t requested = bytes_remaining < FILE_MGR_UPLOAD_BUF_SIZE
            ? bytes_remaining : FILE_MGR_UPLOAD_BUF_SIZE;
        int received = recv_retry_timeout(req, buffer, requested);
        if (received <= 0 ||
            fwrite(buffer, 1u, (size_t)received, file) != (size_t)received) {
            err = ESP_FAIL;
            break;
        }
        bytes_remaining -= (size_t)received;
    }
    free(buffer);
    return err;
}

static esp_err_t file_manager_upload_action(httpd_req_t *req)
{
    size_t metadata_bytes = 0;
    cJSON *root = receive_upload_metadata(req, &metadata_bytes);
    if (root == NULL) {
        return ESP_FAIL;
    }

    char action[16] = {0};
    char fs_type[16] = {0};
    char raw_path[FILE_MGR_MAX_PATH] = {0};
    cJSON *size_item = cJSON_GetObjectItemCaseSensitive(root, "size");
    bool valid = get_required_string(root, "action", action, sizeof(action)) == ESP_OK &&
                 strcmp(action, "upload") == 0 &&
                 get_fs_path(root, fs_type, sizeof(fs_type),
                             raw_path, sizeof(raw_path)) == ESP_OK &&
                 cJSON_IsNumber(size_item) &&
                 size_item->valuedouble >= 0 &&
                 size_item->valuedouble <= (double)SIZE_MAX &&
                 req->content_len >= metadata_bytes;
    size_t payload_bytes = valid ? req->content_len - metadata_bytes : 0u;
    if (!valid ||
        (double)payload_bytes != size_item->valuedouble) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "invalid upload envelope");
        return ESP_FAIL;
    }
    cJSON_Delete(root);

    char resolved[FILE_MGR_MAX_PATH];
    if (validate_and_resolve_path(fs_type, raw_path, resolved,
                                  sizeof(resolved), NULL) != ESP_OK ||
        strcmp(raw_path, "/") == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid path");
        return ESP_FAIL;
    }

    const char *denied = mutation_denied(fs_type, resolved);
    if (denied != NULL) {
        return send_forbidden(req, denied);
    }

    struct stat st;
    if (stat(resolved, &st) == 0) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "path already exists");
    }

    char temporary[FILE_MGR_MAX_PATH + sizeof(FILE_MGR_UPLOAD_TMP_SUFFIX)];
    int temp_len = snprintf(temporary, sizeof(temporary), "%s%s", resolved,
                            FILE_MGR_UPLOAD_TMP_SUFFIX);
    if (temp_len < 0 || (size_t)temp_len >= sizeof(temporary)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "path too long");
        return ESP_FAIL;
    }

    int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        httpd_resp_set_status(req, errno == EEXIST ? "409 Conflict"
                                                   : "500 Internal Server Error");
        return httpd_resp_sendstr(req, errno == EEXIST
            ? "upload already in progress" : "cannot create upload file");
    }
    FILE *file = fdopen(fd, "wb");
    if (file == NULL) {
        close(fd);
        unlink(temporary);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "cannot open upload stream");
        return ESP_FAIL;
    }

    esp_err_t err = stream_upload_body(req, file, payload_bytes);
    if (fclose(file) != 0) {
        err = ESP_FAIL;
    }
    if (err != ESP_OK) {
        unlink(temporary);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "upload failed");
        return ESP_FAIL;
    }

    if (rename(temporary, resolved) != 0) {
        unlink(temporary);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "cannot complete upload");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "uploaded: %s (%u bytes)", resolved,
             (unsigned)payload_bytes);
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "uploaded", true);
    cJSON_AddNumberToObject(response, "size", (double)payload_bytes);
    return send_json_response(req, response);
}

/* ── unified API dispatcher ────────────────────────────────────────── */

static esp_err_t file_manager_api_handler_unlocked(httpd_req_t *req)
{
    char content_type[48] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type,
                                    sizeof(content_type)) == ESP_OK &&
        strncmp(content_type, "application/octet-stream",
                strlen("application/octet-stream")) == 0) {
        return file_manager_upload_action(req);
    }

    cJSON *root = receive_json_request(req);
    if (root == NULL) {
        return ESP_FAIL;
    }

    char action[16] = {0};
    if (get_required_string(root, "action", action, sizeof(action)) != ESP_OK) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "missing action field");
        return ESP_FAIL;
    }

    esp_err_t err;
    if (strcmp(action, "list") == 0) {
        err = file_manager_list_action(req, root);
    } else if (strcmp(action, "download") == 0) {
        err = file_manager_download_action(req, root);
    } else if (strcmp(action, "delete") == 0) {
        err = file_manager_delete_action(req, root);
    } else if (strcmp(action, "mkdir") == 0) {
        err = file_manager_mkdir_action(req, root);
    } else if (strcmp(action, "upload") == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "upload requires binary envelope");
        err = ESP_FAIL;
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown action");
        err = ESP_FAIL;
    }
    cJSON_Delete(root);
    return err;
}

static esp_err_t file_manager_api_handler(httpd_req_t *req)
{
    if (begin_file_access(req) != ESP_OK) return ESP_OK;
    const esp_err_t err = file_manager_api_handler_unlocked(req);
    end_file_access();
    return err;
}

/* ── registration ──────────────────────────────────────────────────── */

esp_err_t file_manager_register(httpd_handle_t server)
{
    if (server == NULL || !s_storage.configured) {
        return ESP_ERR_INVALID_STATE;
    }
    const httpd_uri_t page_uri = {
        .uri = "/files",
        .method = HTTP_GET,
        .handler = file_manager_page_handler,
    };
    const httpd_uri_t api_uri = {
        .uri = "/api/fs",
        .method = HTTP_POST,
        .handler = file_manager_api_handler,
    };
    const httpd_uri_t download_get_uri = {
        .uri = "/api/fs/download",
        .method = HTTP_GET,
        .handler = file_manager_download_get_handler,
    };

    esp_err_t err;
    if ((err = httpd_register_uri_handler(server, &page_uri)) != ESP_OK) return err;
    if ((err = httpd_register_uri_handler(server, &api_uri)) != ESP_OK) return err;
    if ((err = httpd_register_uri_handler(server, &download_get_uri)) != ESP_OK) return err;

    ESP_LOGI(TAG, "file manager registered: page + unified API");
    return ESP_OK;
}
