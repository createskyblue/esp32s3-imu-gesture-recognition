#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

struct cJSON;

esp_err_t web_server_init_and_start(void);
esp_err_t web_server_register_static_fallback(void);

esp_err_t send_json_text(httpd_req_t *req, const char *json);
esp_err_t send_json_object(httpd_req_t *req, struct cJSON *root);
esp_err_t receive_json_body(httpd_req_t *req, char *buffer, size_t buffer_size);
