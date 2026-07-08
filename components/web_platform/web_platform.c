#include "web_platform.h"
#include "file_manager.h"
#include "ota_manager.h"
#include "wifi_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_task.h"

/* ── constants ─────────────────────────────────────────────────────────── */
#define LITTLEFS_BASE_PATH           "/littlefs"
#define LITTLEFS_INDEX_PATH          LITTLEFS_BASE_PATH "/index.html"
#define HTTP_FILE_BUFFER_BYTES       1024u
#define HTTP_JSON_BUFFER_BYTES       512u

static const char *TAG = "WEB_PLATFORM";

/* ── HTTP server handle ────────────────────────────────────────────────── */
static httpd_handle_t s_http_server;

/* ── helpers ───────────────────────────────────────────────────────────── */
static void blue_data_blink(void)
{
    const led_cmd_t cmd = { .led = LED_BLUE, .type = LED_CMD_ONESHOT, .on_ms = 100 };
    led_send_cmd(&cmd);
}

esp_err_t send_json_text(httpd_req_t *req, const char *json)
{
    blue_data_blink();
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    return httpd_resp_sendstr(req, json != NULL ? json : "{}");
}

esp_err_t send_json_object(httpd_req_t *req, cJSON *root)
{
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json allocation failed");
        return ESP_FAIL;
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json allocation failed");
        return ESP_FAIL;
    }
    esp_err_t err = send_json_text(req, json);
    cJSON_free(json);
    return err;
}

esp_err_t receive_json_body(httpd_req_t *req, char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0u || req->content_len >= buffer_size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json body too large");
        return ESP_FAIL;
    }
    size_t received = 0u;
    while (received < req->content_len) {
        const int ret = httpd_req_recv(req, buffer + received, req->content_len - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed to receive body");
            return ESP_FAIL;
        }
        received += (size_t)ret;
    }
    buffer[received] = '\0';
    return ESP_OK;
}

/* ── LittleFS ──────────────────────────────────────────────────────────── */
static esp_err_t littlefs_init(void)
{
    const esp_vfs_littlefs_conf_t conf = {
        .base_path = LITTLEFS_BASE_PATH,
        .partition_label = "storage",
        .format_if_mount_failed = false,
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(err));
        return err;
    }
    size_t total = 0u, used = 0u;
    err = esp_littlefs_info(conf.partition_label, &total, &used);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "LittleFS info failed: %s", esp_err_to_name(err));
    else
        ESP_LOGI(TAG, "LittleFS mounted: total=%u used=%u", (unsigned)total, (unsigned)used);
    return ESP_OK;
}

/* ══════════════════════════════════════════════════════════════════════════
 * HTTP handlers
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── GET / ─────────────────────────────────────────────────────────────── */
static esp_err_t root_handler(httpd_req_t *req)
{
    FILE *file = fopen(LITTLEFS_INDEX_PATH, "r");
    if (file == NULL) {
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
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    return err;
}

/* ── GET catch-all  (static file fallback) ────────────────────────────── */
static esp_err_t littlefs_static_handler(httpd_req_t *req)
{
    char path[576];
    snprintf(path, sizeof(path), "%s%s", LITTLEFS_BASE_PATH, req->uri);

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        /* Redirect unknown paths to / for captive portal detection */
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    const char *type = "application/octet-stream";
    const char *ext = strrchr(req->uri, '.');
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
    cJSON_AddStringToObject(root, "ap_ip", snap.ap_ip);
    cJSON_AddStringToObject(root, "config_path", wifi_manager_get_config_path());
    cJSON_AddBoolToObject(root, "config_loaded", snap.config_loaded);

    const esp_app_desc_t *app_desc = esp_app_get_description();
    char build_ts[32];
    snprintf(build_ts, sizeof(build_ts), "%s %s", app_desc->date, app_desc->time);
    cJSON_AddStringToObject(root, "app_build_id", "esp32s3-template-v1");
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

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json allocation failed");
        return ESP_FAIL;
    }
    cJSON_AddStringToObject(root, "ssid", snap.sta_ssid);
    cJSON_AddBoolToObject(root, "has_password", snap.has_password);
    cJSON_AddStringToObject(root, "path", wifi_manager_get_config_path());
    cJSON_AddBoolToObject(root, "loaded_from_file", snap.config_loaded);
    return send_json_object(req, root);
}

/* ── POST /wifi_config.json ────────────────────────────────────────────── */
static esp_err_t wifi_config_post_handler(httpd_req_t *req)
{
    char body[HTTP_JSON_BUFFER_BYTES];
    if (receive_json_body(req, body, sizeof(body)) != ESP_OK) return ESP_FAIL;

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    cJSON *ssid_item = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    cJSON *pass_item = cJSON_GetObjectItemCaseSensitive(root, "password");
    bool valid = cJSON_IsString(ssid_item) && ssid_item->valuestring != NULL &&
                 ssid_item->valuestring[0] != '\0';
    const char *ssid = valid ? ssid_item->valuestring : NULL;
    const char *pass = cJSON_IsString(pass_item) ? pass_item->valuestring : "";

    if (!valid) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "expected JSON with non-empty ssid and optional password");
        return ESP_FAIL;
    }

    esp_err_t err = wifi_manager_save_credentials(ssid, pass);
    cJSON_Delete(root);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save WiFi config failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed to save wifi config");
        return ESP_FAIL;
    }

    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json allocation failed");
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "ssid", ssid);
    cJSON_AddStringToObject(resp, "path", wifi_manager_get_config_path());
    cJSON_AddStringToObject(resp, "message", "saved; reconnecting STA");
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
    cJSON_AddNumberToObject(root, "internal_free_heap",
                            heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(root, "internal_min_free_heap",
                            heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(root, "internal_largest_free_block",
                            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(root, "psram_free_heap",
                            heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

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
        /* clang-format on */

        esp_err_t reg_err;
        if ((reg_err = httpd_register_uri_handler(server, &root_uri))   != ESP_OK ||
            (reg_err = httpd_register_uri_handler(server, &net_uri))    != ESP_OK ||
            (reg_err = httpd_register_uri_handler(server, &wcfg_g_uri)) != ESP_OK ||
            (reg_err = httpd_register_uri_handler(server, &wcfg_p_uri)) != ESP_OK ||
            (reg_err = httpd_register_uri_handler(server, &debug_uri))  != ESP_OK) {
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
    ESP_ERROR_CHECK(ota_manager_init());
    ESP_ERROR_CHECK(littlefs_init());
    ESP_ERROR_CHECK(wifi_manager_init());
    return start_webserver();
}
