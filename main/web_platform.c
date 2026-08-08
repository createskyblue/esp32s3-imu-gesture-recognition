#include "web_platform.h"
#include "file_manager.h"
#include "ota_manager.h"
#include "app_storage.h"
#include "app_config.h"
#include "wifi_config_store.h"
#include "wifi_manager.h"

#include "json_http.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ── constants ─────────────────────────────────────────────────────────── */
#define LITTLEFS_INDEX_PATH          APP_LITTLEFS_BASE_PATH "/index.html"
#define HTTP_FILE_BUFFER_BYTES       1024u
#define HTTP_JSON_BUFFER_BYTES       512u

static const char *TAG = "WEB_PLATFORM";
static const char FILESYSTEM_RECOVERY_HTML[] =
    "<!doctype html><html lang=\"zh-CN\"><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>文件系统恢复</title><body><h1>文件系统不可用</h1>"
    "<p>设备已进入 AP + OTA 恢复模式。请选择有效的 LittleFS 镜像重新上传。</p>"
    "<input id=\"image\" type=\"file\"><button onclick=\"recover()\">上传并恢复</button>"
    "<pre id=\"status\"></pre><script>async function recover(){const f=image.files[0];"
    "if(!f){status.textContent='请选择镜像';return;}status.textContent='上传中…';"
    "try{const r=await fetch('/ota/upload/filesystem',{method:'POST',body:f});"
    "status.textContent=await r.text();}catch(e){status.textContent=String(e);}}</script>"
    "</body></html>";

/* ── HTTP server handle ────────────────────────────────────────────────── */
static httpd_handle_t s_http_server;
static esp_timer_handle_t s_reboot_timer;

static const char *protect_wifi_config(const char *fs_type, const char *path)
{
    if (fs_type != NULL && strcmp(fs_type, "internal") == 0 &&
        wifi_config_store_is_path(path)) {
        return "WiFi configuration is application-private";
    }
    return NULL;
}

static bool resolve_static_path(const char *uri, char *path, size_t path_size)
{
    if (uri == NULL || path == NULL || path_size == 0u || uri[0] != '/') {
        return false;
    }

    const size_t uri_length = strcspn(uri, "?#");
    if (uri_length <= 1u) return false;

    size_t segment_start = 1u;
    for (size_t i = 1u; i <= uri_length; ++i) {
        if (i != uri_length && uri[i] != '/') continue;

        const size_t segment_length = i - segment_start;
        if (segment_length == 0u ||
            (segment_length == 1u && uri[segment_start] == '.') ||
            (segment_length == 2u && uri[segment_start] == '.' &&
             uri[segment_start + 1u] == '.') ||
            memchr(uri + segment_start, '\\', segment_length) != NULL) {
            return false;
        }
        segment_start = i + 1u;
    }

    const int length = snprintf(path, path_size, "%s%.*s",
                                APP_LITTLEFS_BASE_PATH, (int)uri_length, uri);
    return length >= 0 && (size_t)length < path_size;
}

/* Public helpers kept for custom handlers (see hello_web); delegate to the
 * shared json_http helpers so behavior stays consistent across components. */
esp_err_t send_json_text(httpd_req_t *req, const char *json)
{
    return json_send_text(req, json);
}

esp_err_t send_json_object(httpd_req_t *req, cJSON *root)
{
    return json_send_object(req, root);
}

esp_err_t receive_json_body(httpd_req_t *req, char *buffer, size_t buffer_size)
{
    return json_receive_body(req, buffer, buffer_size);
}

/* ══════════════════════════════════════════════════════════════════════════
 * HTTP handlers
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── GET / ─────────────────────────────────────────────────────────────── */
static esp_err_t root_handler(httpd_req_t *req)
{
    if (app_storage_try_acquire() != ESP_OK) {
        if (ota_manager_is_busy()) {
            httpd_resp_set_status(req, "503 Service Unavailable");
            return httpd_resp_sendstr(req, "filesystem OTA is in progress");
        }
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
        return httpd_resp_sendstr(req, FILESYSTEM_RECOVERY_HTML);
    }

    FILE *file = fopen(LITTLEFS_INDEX_PATH, "r");
    if (file == NULL) {
        app_storage_release();
        ESP_LOGE(TAG, "failed to open %s", LITTLEFS_INDEX_PATH);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "index.html not found");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");

    char buffer[HTTP_FILE_BUFFER_BYTES];
    size_t read_bytes;
    esp_err_t err = ESP_OK;
    while ((read_bytes = fread(buffer, 1u, sizeof(buffer), file)) > 0u) {
        err = httpd_resp_send_chunk(req, buffer, read_bytes);
        if (err != ESP_OK) break;
    }
    fclose(file);
    app_storage_release();
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    return err;
}

/* ── GET catch-all  (static file fallback) ────────────────────────────── */
static esp_err_t littlefs_static_handler(httpd_req_t *req)
{
    char path[576];
    if (!resolve_static_path(req->uri, path, sizeof(path))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid static path");
        return ESP_FAIL;
    }
    if (wifi_config_store_is_path(path)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "file not found");
        return ESP_FAIL;
    }

    if (app_storage_try_acquire() != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "filesystem is temporarily unavailable");
    }

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        app_storage_release();
        /* Redirect unknown paths to / for captive portal detection */
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    const char *type = "application/octet-stream";
    const char *ext = strrchr(path, '.');
    if (ext) {
        if (strcasecmp(ext, ".html") == 0)      type = "text/html; charset=utf-8";
        else if (strcasecmp(ext, ".js") == 0)   type = "application/javascript";
        else if (strcasecmp(ext, ".css") == 0)  type = "text/css";
        else if (strcasecmp(ext, ".json") == 0) type = "application/json";
        else if (strcasecmp(ext, ".svg") == 0)  type = "image/svg+xml";
        else if (strcasecmp(ext, ".png") == 0)  type = "image/png";
        else if (strcasecmp(ext, ".ico") == 0)  type = "image/x-icon";
    }
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");

    char buffer[HTTP_FILE_BUFFER_BYTES];
    size_t read_bytes;
    esp_err_t err = ESP_OK;
    while ((read_bytes = fread(buffer, 1u, sizeof(buffer), file)) > 0u) {
        err = httpd_resp_send_chunk(req, buffer, read_bytes);
        if (err != ESP_OK) break;
    }
    fclose(file);
    app_storage_release();
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    return err;
}

/* ── GET /network.json ─────────────────────────────────────────────────── */
static esp_err_t network_json_handler(httpd_req_t *req)
{
    wifi_snapshot_t snap;
    wifi_manager_get_snapshot(&snap);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json allocation failed");
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(root, "sta_connected", snap.sta_connected);
    cJSON_AddStringToObject(root, "sta_ssid", snap.sta_ssid);
    cJSON_AddStringToObject(root, "sta_ip", snap.sta_connected ? snap.sta_ip : "0.0.0.0");
    cJSON_AddStringToObject(root, "ap_ssid", wifi_manager_get_ap_ssid());
    cJSON_AddStringToObject(root, "ap_password", wifi_manager_get_ap_password());
    cJSON_AddStringToObject(root, "ap_ip", snap.ap_ip);
    cJSON_AddStringToObject(root, "config_path", wifi_config_store_get_path());
    cJSON_AddBoolToObject(root, "config_exists", wifi_config_store_exists());

    const esp_app_desc_t *app_desc = esp_app_get_description();
    char build_ts[32];
    snprintf(build_ts, sizeof(build_ts), "%s %s", app_desc->date, app_desc->time);
    cJSON_AddStringToObject(root, "app_build_id", APP_BUILD_ID);
    cJSON_AddStringToObject(root, "firmware_sha256", esp_app_get_elf_sha256_str());
    cJSON_AddStringToObject(root, "build_timestamp", build_ts);
    cJSON_AddStringToObject(root, "idf_version", app_desc->idf_ver);
    return send_json_object(req, root);
}

/* ── GET /wifi_config.json ─────────────────────────────────────────────── */
static esp_err_t wifi_config_get_handler(httpd_req_t *req)
{
    wifi_snapshot_t snap;
    wifi_manager_get_snapshot(&snap);

    /* Load the persisted config so the page can prefill the settings;
     * fall back to the runtime snapshot (empty fields) when the filesystem
     * is busy or the file is missing/corrupt. */
    wifi_persisted_config_t saved = {0};
    bool have_saved = false;
    if (app_storage_try_acquire() == ESP_OK) {
        have_saved = wifi_config_store_load_full(&saved) == ESP_OK;
        app_storage_release();
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json allocation failed");
        return ESP_FAIL;
    }
    cJSON_AddStringToObject(root, "ssid", have_saved ? saved.sta.sta_ssid : snap.sta_ssid);
    cJSON_AddBoolToObject(root, "has_password", snap.has_password);
    cJSON_AddStringToObject(root, "ip_mode",
                            have_saved && saved.sta.ip_static ? "static" : "dhcp");
    cJSON_AddStringToObject(root, "static_ip", have_saved ? saved.sta.ip_addr : "");
    cJSON_AddStringToObject(root, "netmask", have_saved ? saved.sta.ip_netmask : "");
    cJSON_AddStringToObject(root, "gateway", have_saved ? saved.sta.ip_gateway : "");
    cJSON_AddStringToObject(root, "dns", have_saved ? saved.sta.ip_dns : "");
    cJSON_AddStringToObject(root, "ap_ssid",
                            have_saved && saved.ap_ssid[0] != '\0'
                                ? saved.ap_ssid : wifi_manager_get_ap_ssid());
    cJSON_AddStringToObject(root, "ap_password",
                            have_saved && saved.ap_ssid[0] != '\0'
                                ? saved.ap_password : wifi_manager_get_ap_password());
    cJSON_AddStringToObject(root, "path", wifi_config_store_get_path());
    cJSON_AddBoolToObject(root, "config_exists", wifi_config_store_exists());
    return send_json_object(req, root);
}

/* ── POST /wifi_config.json ────────────────────────────────────────────── */
static esp_err_t wifi_config_post_handler(httpd_req_t *req)
{
    if (ota_manager_is_busy()) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req,
                                  "cannot update WiFi configuration during OTA");
    }

    char body[HTTP_JSON_BUFFER_BYTES];
    if (receive_json_body(req, body, sizeof(body)) != ESP_OK) return ESP_FAIL;

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    /* Partial update: load the persisted config as the base so a request can
     * change only the WiFi credentials, IP policy, or AP identity without
     * touching the others, and without wiping a previously saved password. */
    wifi_persisted_config_t config = {0};
    if (app_storage_try_acquire() == ESP_OK) {
        (void)wifi_config_store_load_full(&config);
        app_storage_release();
    }

    cJSON *ssid_item = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    cJSON *pass_item = cJSON_GetObjectItemCaseSensitive(root, "password");
    cJSON *ip_mode_item = cJSON_GetObjectItemCaseSensitive(root, "ip_mode");
    cJSON *ap_ssid_item = cJSON_GetObjectItemCaseSensitive(root, "ap_ssid");
    cJSON *ap_pass_item = cJSON_GetObjectItemCaseSensitive(root, "ap_password");
    const bool have_ssid = cJSON_IsString(ssid_item) &&
                           ssid_item->valuestring != NULL &&
                           ssid_item->valuestring[0] != '\0';
    const bool have_ip_mode = cJSON_IsString(ip_mode_item);
    const bool have_ap_ssid = cJSON_IsString(ap_ssid_item) &&
                              ap_ssid_item->valuestring != NULL &&
                              ap_ssid_item->valuestring[0] != '\0';
    const bool updated_wifi = have_ssid;
    const bool updated_ap = have_ap_ssid;
    if (!have_ssid && !have_ip_mode && !have_ap_ssid) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "provide ssid/password, ip_mode, and/or ap_ssid");
        return ESP_FAIL;
    }

    bool valid = true;
    if (have_ssid) {
        const size_t ssid_len = strlen(ssid_item->valuestring);
        if (ssid_len > WIFI_MANAGER_SSID_MAX_BYTES) {
            valid = false;
        } else {
            memcpy(config.sta.sta_ssid, ssid_item->valuestring, ssid_len + 1u);
            if (pass_item != NULL && cJSON_IsString(pass_item) &&
                pass_item->valuestring != NULL && pass_item->valuestring[0] != '\0') {
                const size_t pass_len = strlen(pass_item->valuestring);
                if (pass_len > WIFI_MANAGER_PASSWORD_MAX_BYTES) {
                    valid = false;
                } else {
                    memcpy(config.sta.sta_password, pass_item->valuestring,
                           pass_len + 1u);
                }
            }
            /* empty password keeps any previously saved one */
        }
    }

    if (have_ip_mode) {
        if (strcmp(ip_mode_item->valuestring, "static") == 0) {
            config.sta.ip_static = true;
            const char *const keys[4] = { "static_ip", "netmask", "gateway", "dns" };
            char *const fields[4] = { config.sta.ip_addr,
                                      config.sta.ip_netmask,
                                      config.sta.ip_gateway,
                                      config.sta.ip_dns };
            for (int i = 0; i < 4; ++i) {
                cJSON *item = cJSON_GetObjectItemCaseSensitive(root, keys[i]);
                if (!cJSON_IsString(item) || item->valuestring == NULL ||
                    item->valuestring[0] == '\0' ||
                    strlen(item->valuestring) > WIFI_MANAGER_IP_MAX_BYTES ||
                    !wifi_manager_ipv4_is_valid(item->valuestring)) {
                    valid = false;
                    break;
                }
                memcpy(fields[i], item->valuestring, strlen(item->valuestring) + 1u);
            }
        } else if (strcmp(ip_mode_item->valuestring, "dhcp") == 0) {
            /* Keep the previous static values so switching back to static
             * restores them; they are ignored while ip_static is false. */
            config.sta.ip_static = false;
        } else {
            valid = false;
        }
    }

    if (have_ap_ssid) {
        const size_t ap_ssid_len = strlen(ap_ssid_item->valuestring);
        if (ap_ssid_len > WIFI_MANAGER_SSID_MAX_BYTES) {
            valid = false;
        } else {
            memcpy(config.ap_ssid, ap_ssid_item->valuestring, ap_ssid_len + 1u);
            if (ap_pass_item != NULL && cJSON_IsString(ap_pass_item) &&
                ap_pass_item->valuestring != NULL &&
                ap_pass_item->valuestring[0] != '\0') {
                const size_t ap_pass_len = strlen(ap_pass_item->valuestring);
                if (ap_pass_len > WIFI_MANAGER_PASSWORD_MAX_BYTES ||
                    ap_pass_len < 8u) {
                    valid = false;
                } else {
                    memcpy(config.ap_password, ap_pass_item->valuestring,
                           ap_pass_len + 1u);
                }
            }
            /* empty AP password keeps any previously saved one */
        }
    }

    cJSON_Delete(root);

    if (!valid || config.sta.sta_ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            config.sta.sta_ssid[0] == '\0'
                                ? "no saved WiFi config; provide ssid first"
                                : "invalid settings");
        return ESP_FAIL;
    }

    if (app_storage_try_acquire() != ESP_OK) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req,
                                  "filesystem update is already in progress");
    }
    if (ota_manager_is_busy()) {
        app_storage_release();
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req,
                                  "cannot update WiFi configuration during OTA");
    }

    /* The transactional stage → apply → commit → rollback lives in the store. */
    esp_err_t err = wifi_config_store_apply_full(&config);
    app_storage_release();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "apply WiFi config failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "failed to save wifi config");
        return ESP_FAIL;
    }

    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json allocation failed");
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "ssid", config.sta.sta_ssid);
    cJSON_AddStringToObject(resp, "path", wifi_config_store_get_path());
    cJSON_AddStringToObject(resp, "message",
                            updated_wifi ? "saved; reconnecting STA"
                            : (updated_ap ? "AP identity saved"
                                          : "IP settings saved; reconnecting STA"));
    /* The address the STA will move to after the deferred apply, so the page
     * can redirect there; empty for DHCP (address unknown until renewal). */
    cJSON_AddStringToObject(resp, "new_ip",
                            config.sta.ip_static ? config.sta.ip_addr : "");
    return send_json_object(req, resp);
}

/* ── GET /debug.json ───────────────────────────────────────────────────── */
static esp_err_t debug_json_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json allocation failed");
        return ESP_FAIL;
    }

    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "min_free_heap", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(root, "largest_free_block",
                            heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    const uint32_t internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const size_t internal_total = heap_caps_get_total_size(internal_caps);
    const size_t internal_free = heap_caps_get_free_size(internal_caps);
    const size_t internal_used = internal_total >= internal_free
                                     ? internal_total - internal_free
                                     : 0u;
    cJSON_AddNumberToObject(root, "internal_total_heap", internal_total);
    cJSON_AddNumberToObject(root, "internal_used_heap", internal_used);
    cJSON_AddNumberToObject(root, "internal_free_heap", internal_free);
    cJSON_AddNumberToObject(root, "internal_min_free_heap",
                            heap_caps_get_minimum_free_size(internal_caps));
    cJSON_AddNumberToObject(root, "internal_largest_free_block",
                            heap_caps_get_largest_free_block(internal_caps));
    const uint32_t psram_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    const size_t psram_total = heap_caps_get_total_size(psram_caps);
    const size_t psram_free = heap_caps_get_free_size(psram_caps);
    const size_t psram_used = psram_total >= psram_free
                                  ? psram_total - psram_free
                                  : 0u;
    cJSON_AddNumberToObject(root, "psram_total_heap", psram_total);
    cJSON_AddNumberToObject(root, "psram_used_heap", psram_used);
    cJSON_AddNumberToObject(root, "psram_free_heap", psram_free);

#if configUSE_TRACE_FACILITY && configUSE_STATS_FORMATTING_FUNCTIONS
    char *task_buf = malloc(2048);
    if (task_buf != NULL) {
        int hdr = snprintf(task_buf, 2048,
                           "名称            状态  优先级  栈剩余  序号\r\n"
                           "------------------------------------------------\r\n");
        if (hdr > 0 && hdr < 2048) vTaskList(task_buf + hdr);
        cJSON_AddStringToObject(root, "task_list", task_buf);
        free(task_buf);
    }
#else
    cJSON_AddStringToObject(root, "task_list",
        "(需要启用 CONFIG_FREERTOS_USE_TRACE_FACILITY 和 CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS)");
#endif

    cJSON_AddNumberToObject(root, "uptime_ms", esp_timer_get_time() / 1000);
    return send_json_object(req, root);
}

/* ── POST /reboot ─────────────────────────────────────────────────────── */
static void reboot_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "scheduled reboot firing");
    esp_restart();
}

static esp_err_t reboot_handler(httpd_req_t *req)
{
    if (s_reboot_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = reboot_timer_cb,
            .name = "reboot_delay",
        };
        esp_err_t err = esp_timer_create(&args, &s_reboot_timer);
        if (err != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "cannot create reboot timer");
            return ESP_FAIL;
        }
    } else {
        (void)esp_timer_stop(s_reboot_timer);
    }

    esp_err_t err = esp_timer_start_once(s_reboot_timer, 3000ULL * 1000ULL);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "cannot schedule reboot");
        return ESP_FAIL;
    }

    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "json allocation failed");
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "message", "rebooting in 3 seconds");
    return send_json_object(req, resp);
}

/* ══════════════════════════════════════════════════════════════════════════
 * HTTP server setup
 * ══════════════════════════════════════════════════════════════════════════ */

static esp_err_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 16;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;
    config.stack_size = 16384;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        /* clang-format off */
        httpd_uri_t root_uri  = { .uri = "/",              .method = HTTP_GET,  .handler = root_handler };
        httpd_uri_t net_uri   = { .uri = "/network.json",  .method = HTTP_GET,  .handler = network_json_handler };
        httpd_uri_t wcfg_g_uri= { .uri = "/wifi_config.json", .method = HTTP_GET, .handler = wifi_config_get_handler };
        httpd_uri_t wcfg_p_uri= { .uri = "/wifi_config.json", .method = HTTP_POST,.handler = wifi_config_post_handler };
        httpd_uri_t debug_uri = { .uri = "/debug.json",    .method = HTTP_GET,  .handler = debug_json_handler };
        httpd_uri_t reboot_uri= { .uri = "/reboot",        .method = HTTP_POST, .handler = reboot_handler };
        /* clang-format on */

        esp_err_t reg_err;
        if ((reg_err = httpd_register_uri_handler(server, &root_uri))   != ESP_OK ||
            (reg_err = httpd_register_uri_handler(server, &net_uri))    != ESP_OK ||
            (reg_err = httpd_register_uri_handler(server, &wcfg_g_uri)) != ESP_OK ||
            (reg_err = httpd_register_uri_handler(server, &wcfg_p_uri)) != ESP_OK ||
            (reg_err = httpd_register_uri_handler(server, &debug_uri))  != ESP_OK ||
            (reg_err = httpd_register_uri_handler(server, &reboot_uri)) != ESP_OK) {
            ESP_LOGE(TAG, "URI handler registration failed: %s", esp_err_to_name(reg_err));
        }
        if (file_manager_register(server) != ESP_OK) {
            ESP_LOGE(TAG, "file manager registration failed");
        }
        if (ota_manager_register(server) != ESP_OK) {
            ESP_LOGE(TAG, "OTA handler registration failed");
        }

        /* Static file fallback (catch-all) is NOT registered here — callers must
         * invoke web_platform_register_static_fallback() LAST so exact URIs
         * match before the wildcard. */
        s_http_server = server;
        ESP_LOGI(TAG, "HTTP server started on port 80");
    }
    return server == NULL ? ESP_FAIL : ESP_OK;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Public API
 * ══════════════════════════════════════════════════════════════════════════ */

httpd_handle_t web_platform_get_server(void)
{
    return s_http_server;
}

esp_err_t web_platform_register_static_fallback(void)
{
    if (s_http_server == NULL) return ESP_ERR_INVALID_STATE;
    static const httpd_uri_t static_uri = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = littlefs_static_handler,
    };
    esp_err_t err = httpd_register_uri_handler(s_http_server, &static_uri);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "Static file fallback registered (/*)");
    return err;
}

esp_err_t web_platform_init(void)
{
    const file_manager_storage_config_t file_manager_config = {
        .internal_mount_point = APP_LITTLEFS_BASE_PATH,
        .internal_partition_label = APP_LITTLEFS_PARTITION_LABEL,
        .sd_mount_point = NULL,
    };
    const ota_manager_config_t ota_config = {
        .filesystem_partition_label = APP_LITTLEFS_PARTITION_LABEL,
        .filesystem_update_begin = app_storage_begin_update,
        .filesystem_update_end = app_storage_end_update,
    };
    ESP_ERROR_CHECK(file_manager_set_storage_config(&file_manager_config));
    ESP_ERROR_CHECK(ota_manager_init_with_config(&ota_config));
    file_manager_set_access_callbacks(app_storage_try_acquire,
                                      app_storage_release);
    file_manager_set_read_guard(protect_wifi_config);
    file_manager_set_mutation_guard(protect_wifi_config);
    return start_webserver();
}
