#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>

#include "app_storage.h"
#include "lcd_lvgl.h"
#include "gesture_demo.h"
#include "sd_card.h"
#include "web_platform.h"
#include "wifi_config_store.h"
#include "wifi_manager.h"

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


/* Simple SD card read/write self-test: write a file, read it back, compare. */
static void sd_card_rw_selftest(void)
{
    const char *path = "/sdcard/lckfb_sd_test.txt";
    const char *payload = "LCKFB ESP32-S3 SD card R/W test OK\n";
    char buf[160] = {0};

    FILE *f = fopen(path, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "SD test: cannot open %s for write", path);
        return;
    }
    const size_t written = fwrite(payload, 1, strlen(payload), f);
    fclose(f);
    if (written != strlen(payload)) {
        ESP_LOGE(TAG, "SD test: short write (%d/%d bytes)",
                 (int)written, (int)strlen(payload));
        return;
    }

    f = fopen(path, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "SD test: cannot open %s for read", path);
        return;
    }
    const size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[got] = '\0';

    if (got == strlen(payload) && strcmp(buf, payload) == 0) {
        ESP_LOGI(TAG, "SD R/W test PASS (%d bytes at %s)", (int)got, path);
    } else {
        ESP_LOGE(TAG, "SD R/W test FAIL: got %d bytes: '%s'", (int)got, buf);
    }
}

void app_main(void)
{
    const esp_err_t storage_err = app_storage_init();
    if (storage_err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS unavailable: %s; starting AP + OTA recovery mode",
                 esp_err_to_name(storage_err));
    }

    /* SD card (LCKFB board, SDMMC 1-bit): init + simple R/W self-test */
    if (sd_card_init() == ESP_OK) {
        sd_card_rw_selftest();
    } else {
        ESP_LOGW(TAG, "SD card init failed; skip R/W self-test");
    }

    /* 立创实战派 LCD (ST7789) + LVGL 9.5: 先注册手势演示 UI，再创建显示任务 */
    lcd_lvgl_set_screen_builder(gesture_demo_ui_create);
    esp_err_t lcd_err = lcd_lvgl_start();
    if (lcd_err != ESP_OK) {
        ESP_LOGE(TAG, "LCD/LVGL start failed: %s", esp_err_to_name(lcd_err));
    }

    /* 加速度计手势识别：IMU + BOOT 按键 + CSV 采集（依赖 lcd_lvgl 的 I2C 总线） */
    esp_err_t gesture_err = gesture_demo_start();
    if (gesture_err != ESP_OK) {
        ESP_LOGE(TAG, "Gesture demo start failed: %s", esp_err_to_name(gesture_err));
    }

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
    ESP_LOGI(TAG, "Mem: internal free=%u B, psram free=%u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
