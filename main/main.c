#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_storage.h"
#include "led_task.h"
#include "web_platform.h"
#include "wifi_config_store.h"
#include "wifi_manager.h"
#if CONFIG_BLUFI_PROVISIONING_ENABLED
#include "ble_echo.h"
#include "blufi_provisioning.h"
#endif

static const char *TAG = "MAIN";

/* Base SoftAP SSID for boards running this template. build_default_ap_ssid()
 * appends a short MAC-derived suffix so multiple boards can be told apart on
 * the WiFi scanner; a persisted custom AP identity overrides it in app_main(). */
#define DEFAULT_AP_SSID_BASE "ESP32S3-Template"

static void build_default_ap_ssid(char *buf, size_t buf_size)
{
    uint8_t mac[6] = {0};
    if (buf_size == 0u) return;
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        snprintf(buf, buf_size, "%s", DEFAULT_AP_SSID_BASE);
        return;
    }
    snprintf(buf, buf_size, "%s-%02X%02X%02X",
             DEFAULT_AP_SSID_BASE, mac[3], mac[4], mac[5]);
}

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
        .ap_ssid = DEFAULT_AP_SSID_BASE,
        .ap_password = "template1234",
        .ap_channel = 6u,
        .ap_max_connections = 4u,
        .captive_portal_dns_enabled = true,
        .sntp_server = "ntp.aliyun.com",
    };

    /* Per-device default SoftAP identity: append a MAC-derived suffix so
     * boards flashed with this template are distinguishable. A persisted
     * custom AP identity (below) still overrides it. */
    char default_ap_ssid[WIFI_MANAGER_SSID_MAX_BYTES + 1u];
    build_default_ap_ssid(default_ap_ssid, sizeof(default_ap_ssid));
    snprintf(wifi_config.ap_ssid, sizeof(wifi_config.ap_ssid),
             "%s", default_ap_ssid);

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
     * otherwise keep the compiled-in defaults above. A persisted SSID equal to
     * the unsuffixed legacy default is treated as "not customized", so boards
     * that upgraded from older firmware still get the unique per-device name. */
    if (config_err == ESP_OK) {
        wifi_persisted_config_t full = {0};
        if (wifi_config_store_load_full(&full) == ESP_OK &&
            full.ap_ssid[0] != '\0' &&
            strcmp(full.ap_ssid, DEFAULT_AP_SSID_BASE) != 0) {
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

    /* ── BluFi (BLE) 配网 ───────────────────────────────────── */
    /* 编译开关 CONFIG_BLUFI_PROVISIONING_ENABLED（见 main/Kconfig.projbuild）：
     * 开启会引入整个蓝牙栈，常驻约 60 KB SRAM；关闭则完全不编译 BLE。 */
#if CONFIG_BLUFI_PROVISIONING_ENABLED
    blufi_provisioning_config_t blufi_cfg = {
        .apply_credentials = wifi_config_store_apply_credentials,
    };
    /* BLE 设备名与 SoftAP 名保持一致（含 MAC 后缀），便于辨认 */
    snprintf(blufi_cfg.device_name, sizeof(blufi_cfg.device_name),
             "%s", wifi_manager_get_ap_ssid());
    esp_err_t blufi_err = blufi_provisioning_init(&blufi_cfg);
    if (blufi_err != ESP_OK) {
        ESP_LOGW(TAG, "BluFi provisioning init failed: %s; BLE 配网不可用",
                 esp_err_to_name(blufi_err));
    }

    /* BLE echo 示例：演示在 BLE 上挂载自定义 GATT 服务（与 BluFi 配网并存） */
    if (ble_echo_init() != ESP_OK) {
        ESP_LOGW(TAG, "BLE echo init failed");
    }
#endif

    /* ── 平台基础 Web 服务（HTTP + OTA + 文件管理） ───────── */
    ESP_ERROR_CHECK(web_platform_init());

    /* ── 静态文件回退 ── 必须最后注册 ──────────────────────── */
    ESP_ERROR_CHECK(web_platform_register_static_fallback());

    ESP_LOGI(TAG, "all tasks started");
}
