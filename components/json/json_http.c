#include "json_http.h"

#include <stdio.h>

#include "cJSON.h"
#include "esp_http_server.h"

void json_copy_str(char *dest, size_t dest_size, const char *src)
{
    if (dest == NULL || dest_size == 0u) return;
    if (src == NULL) { dest[0] = '\0'; return; }
    snprintf(dest, dest_size, "%s", src);
}

esp_err_t json_receive_body(httpd_req_t *req, char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0u || req->content_len >= buffer_size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json body too large");
        return ESP_FAIL;
    }
    size_t received = 0u;
    while (received < req->content_len) {
        const int ret = httpd_req_recv(req, buffer + received,
                                       req->content_len - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "failed to receive body");
            return ESP_FAIL;
        }
        received += (size_t)ret;
    }
    buffer[received] = '\0';
    return ESP_OK;
}

esp_err_t json_send_text(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    return httpd_resp_sendstr(req, json != NULL ? json : "{}");
}

esp_err_t json_send_object(httpd_req_t *req, cJSON *root)
{
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "json allocation failed");
        return ESP_FAIL;
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "json allocation failed");
        return ESP_FAIL;
    }
    esp_err_t err = json_send_text(req, json);
    cJSON_free(json);
    return err;
}
