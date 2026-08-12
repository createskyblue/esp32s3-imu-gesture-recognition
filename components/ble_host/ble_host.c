#include "ble_host.h"

#include <string.h>

#include "esp_bt.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"

/* NimBLE 配置存储初始化（头文件未导出，按官方示例声明） */
void ble_store_config_init(void);

#define BLE_HOST_TAG       "BLE_HOST"
#define BLE_HOST_MAX_HOOKS 8u

static ble_host_config_t s_config;
static const char *s_pre_enable_names[BLE_HOST_MAX_HOOKS];
static esp_err_t (*s_pre_enable[BLE_HOST_MAX_HOOKS])(void);
static uint8_t s_pre_enable_n;
static const char *s_on_sync_names[BLE_HOST_MAX_HOOKS];
static void (*s_on_sync[BLE_HOST_MAX_HOOKS])(void);
static uint8_t s_on_sync_n;

esp_err_t ble_host_register_pre_enable(const char *name, esp_err_t (*cb)(void))
{
    if (name == NULL || cb == NULL) return ESP_ERR_INVALID_ARG;
    if (s_pre_enable_n >= BLE_HOST_MAX_HOOKS) {
        ESP_LOGE(BLE_HOST_TAG, "pre-enable hook table full (max %u)", BLE_HOST_MAX_HOOKS);
        return ESP_ERR_NO_MEM;
    }
    s_pre_enable_names[s_pre_enable_n] = name;
    s_pre_enable[s_pre_enable_n++] = cb;
    return ESP_OK;
}

esp_err_t ble_host_register_on_sync(const char *name, void (*cb)(void))
{
    if (name == NULL || cb == NULL) return ESP_ERR_INVALID_ARG;
    if (s_on_sync_n >= BLE_HOST_MAX_HOOKS) {
        ESP_LOGE(BLE_HOST_TAG, "on-sync hook table full (max %u)", BLE_HOST_MAX_HOOKS);
        return ESP_ERR_NO_MEM;
    }
    s_on_sync_names[s_on_sync_n] = name;
    s_on_sync[s_on_sync_n++] = cb;
    return ESP_OK;
}

static void ble_host_on_reset(int reason)
{
    ESP_LOGW(BLE_HOST_TAG, "NimBLE reset, reason=%d", reason);
}

/* host sync 后：设广播名，再执行各功能的 on_sync 钩子 */
static void ble_host_on_sync(void)
{
    if (s_config.device_name[0] != '\0') {
        ble_svc_gap_device_name_set(s_config.device_name);
    }
    ESP_LOGI(BLE_HOST_TAG, "host synced, GAP name='%s'",
             s_config.device_name[0] ? s_config.device_name : "(default)");
    for (uint8_t i = 0; i < s_on_sync_n; i++) {
        ESP_LOGD(BLE_HOST_TAG, "on-sync hook '%s'", s_on_sync_names[i]);
        s_on_sync[i]();
    }
}

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_host_init(const ble_host_config_t *config)
{
    if (config == NULL) return ESP_ERR_INVALID_ARG;
    s_config = *config;

    /* 1. BT controller（BLE only） */
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_bt_controller_init(&bt_cfg), BLE_HOST_TAG,
                        "BT controller init failed");
    ESP_RETURN_ON_ERROR(esp_bt_controller_enable(ESP_BT_MODE_BLE), BLE_HOST_TAG,
                        "BT controller enable failed");

    /* 2. NimBLE host */
    ESP_RETURN_ON_ERROR(esp_nimble_init(), BLE_HOST_TAG, "NimBLE init failed");
    ble_hs_cfg.reset_cb = ble_host_on_reset;
    ble_hs_cfg.sync_cb = ble_host_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = 4;   /* 无输入输出 → just-works 配对 */
    ble_hs_cfg.sm_sc = 0;
    ble_store_config_init();

    /* 3. enable 前：各功能注册 GATT 服务 / 配置 host（NimBLE 要求 sync 前完成） */
    for (uint8_t i = 0; i < s_pre_enable_n; i++) {
        const esp_err_t err = s_pre_enable[i]();
        if (err != ESP_OK) {
            ESP_LOGW(BLE_HOST_TAG, "pre-enable hook '%s' failed: %s",
                     s_pre_enable_names[i], esp_err_to_name(err));
        }
    }

    ESP_RETURN_ON_ERROR(esp_nimble_enable(ble_host_task), BLE_HOST_TAG,
                        "NimBLE enable failed");
    ESP_LOGI(BLE_HOST_TAG, "host ready, device name: %s",
             s_config.device_name[0] ? s_config.device_name : "(default)");
    return ESP_OK;
}
