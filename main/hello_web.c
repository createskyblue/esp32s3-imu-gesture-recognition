#include "hello_web.h"
#include "web_platform.h"

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "HELLO";

/* ── GET /hello ───────────────────────────────────────────────────────────
 * 返回一个简单的 JSON，演示如何定义和注册自定义 HTTP handler。
 * 这是你的业务代码起点——从这里开始添加自己的端点。
 */
static esp_err_t hello_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }
    cJSON_AddStringToObject(root, "message", "Hello from ESP32-S3!");
    cJSON_AddNumberToObject(root, "uptime_ms", esp_timer_get_time() / 1000);
    return send_json_object(req, root);
}

esp_err_t hello_web_register(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const httpd_uri_t hello_uri = {
        .uri = "/hello",
        .method = HTTP_GET,
        .handler = hello_handler,
    };

    esp_err_t err = httpd_register_uri_handler(server, &hello_uri);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "/hello endpoint registered");
    } else {
        ESP_LOGE(TAG, "/hello registration failed: %s", esp_err_to_name(err));
    }
    return err;
}
