#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

struct cJSON;

/**
 * Start the HTTP server with all platform handlers (/network.json,
 * /debug.json, /ota/..., /wifi_config.json, /, file manager).
 * app_storage_init() and wifi_manager_init() must already have succeeded.
 *
 * The static file fallback (catch-all) is NOT registered — call
 * web_platform_register_static_fallback() after any custom handlers.
 */
esp_err_t web_platform_init(void);

/**
 * Return the HTTP server handle so callers can register custom URI handlers
 * between web_platform_init() and web_platform_register_static_fallback().
 */
httpd_handle_t web_platform_get_server(void);

/**
 * Register the LittleFS static file fallback handler on the catch-all path.
 * Must be called LAST — after all platform and custom handlers are registered.
 */
esp_err_t web_platform_register_static_fallback(void);

/* ── JSON response helpers (usable by custom handlers) ───────────────── */

esp_err_t send_json_text(httpd_req_t *req, const char *json);
esp_err_t send_json_object(httpd_req_t *req, struct cJSON *root);
esp_err_t receive_json_body(httpd_req_t *req, char *buffer, size_t buffer_size);
