#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

struct cJSON;

/**
 * Shared HTTP/JSON helpers for web-facing components.
 *
 * Centralizes the "receive a bounded JSON body" and "cJSON print + send"
 * logic that was previously duplicated across web_platform, ota_manager and
 * file_manager, so error messages and headers stay consistent.
 */

/**
 * Receive the whole request body into buffer.
 * buffer must be at least req->content_len + 1 bytes. Retries on socket
 * timeouts; on failure sends an error response and returns ESP_FAIL.
 */
esp_err_t json_receive_body(httpd_req_t *req, char *buffer, size_t buffer_size);

/** Send a pre-formatted JSON string with a JSON content type and no-store. */
esp_err_t json_send_text(httpd_req_t *req, const char *json);

/** Print, delete and send a cJSON object as the HTTP response. */
esp_err_t json_send_object(httpd_req_t *req, struct cJSON *root);

/** Copy a NUL-terminated string into a fixed-size buffer (snprintf helper). */
void json_copy_str(char *dest, size_t dest_size, const char *src);
