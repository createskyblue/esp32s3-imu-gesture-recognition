#include "esp_err.h"
#include "esp_log.h"

#include <stdlib.h>
#include <time.h>

#include "app_storage.h"
#include "led_task.h"
#include "web_platform.h"
#include "wifi_config_store.h"
#include "wifi_manager.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    const esp_err_t storage_err = app_storage_init();
    if (storage_err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS unavailable: %s; starting AP + OTA recovery mode",
                 esp_err_to_name(storage_err));
    }

    ESP_ERROR_CHECK(led_task_init());
    const led_cmd_t heartbeat = {
        .led = LED_GREEN,
        .type = LED_CMD_BLINK,
        .period_ms = 500u,
        .on_ms = 250u,
    };
    led_send_cmd(&heartbeat);

    /* Set timezone to UTC+8 (China Standard Time) */
    setenv("TZ", "CST-8", 1);
    tzset();

    wifi_manager_config_t wifi_config = {
        .ap_ssid = "ESP32S3-Template",
        .ap_password = "template1234",
        .ap_channel = 6u,
        .ap_max_connections = 4u,
        .captive_portal_dns_enabled = true,
        .sntp_server = "ntp.aliyun.com",
    };
    esp_err_t config_err = storage_err == ESP_OK
        ? wifi_config_store_load(&wifi_config.sta) : storage_err;
    if (config_err == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "WiFi config not found at %s; starting provisioning AP",
                 wifi_config_store_get_path());
    } else if (config_err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi config load failed: %s; starting provisioning AP",
                 esp_err_to_name(config_err));
    }

    /* AP identity (SSID/password) may also be persisted; use it when present,
     * otherwise keep the compiled-in defaults above. */
    if (config_err == ESP_OK) {
        wifi_persisted_config_t full = {0};
        if (wifi_config_store_load_full(&full) == ESP_OK &&
            full.ap_ssid[0] != '\0') {
            snprintf(wifi_config.ap_ssid, sizeof(wifi_config.ap_ssid),
                     "%s", full.ap_ssid);
            snprintf(wifi_config.ap_password, sizeof(wifi_config.ap_password),
                     "%s", full.ap_password);
        }
    }
    esp_err_t wifi_err = wifi_manager_init(&wifi_config);
    if (wifi_err != ESP_OK) {
        if (!wifi_manager_is_started()) {
            ESP_LOGE(TAG, "WiFi initialization failed: %s",
                     esp_err_to_name(wifi_err));
            led_fatal_error();
            return;
        }
        ESP_LOGW(TAG, "WiFi initialization incomplete: %s; provisioning AP remains active",
                 esp_err_to_name(wifi_err));
    }

    /* ── 平台基础 Web 服务（HTTP + OTA + 文件管理） ───────── */
    ESP_ERROR_CHECK(web_platform_init());

    /* ── 静态文件回退 ── 必须最后注册 ──────────────────────── */
    ESP_ERROR_CHECK(web_platform_register_static_fallback());

    ESP_LOGI(TAG, "all tasks started");
}
