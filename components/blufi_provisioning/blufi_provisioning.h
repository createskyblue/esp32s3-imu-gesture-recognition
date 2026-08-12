#pragma once

#include "esp_err.h"
#include "wifi_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLUFI_PROVISIONING_DEVICE_NAME_MAX_BYTES 32u

/** Caller-owned BluFi provisioning policy, copied by blufi_provisioning_init(). */
typedef struct {
    /** BLE 广播/设备名（随 SoftAP 一起广播，缺省为空串则用 BluFi 默认名）。 */
    char device_name[BLUFI_PROVISIONING_DEVICE_NAME_MAX_BYTES + 1u];
    /** STA 凭据落地回调：应用凭据 + 原子持久化 + 失败回滚。由应用层提供，
     *  例如 wifi_config_store_apply_credentials()。不可为空。 */
    esp_err_t (*apply_credentials)(const wifi_manager_credentials_t *credentials);
    /** 可选：NimBLE host 初始化后、enable 前回调。应用在这里注册额外的 GATT
     *  服务（如 ble_echo）—— NimBLE 要求所有服务在 host sync 前配置好。
     *  返回非 ESP_OK 会上报日志。NULL 忽略。 */
    esp_err_t (*register_services_cb)(void);
} blufi_provisioning_config_t;

/**
 * 与 BLE 主机/从机共存（NimBLE 模型）：
 * NimBLE 的 GAP 事件回调是"每次操作自带"的（ble_gap_disc / ble_gap_connect /
 * 广播各自携带回调），没有 Bluedroid 那种全局单槽。所以其它 BLE 功能（主机扫描、
 * 从机服务）直接注册自己的 handler 即可，与本组件无冲突；GATT 服务器/客户端各自独立。
 */

/**
 * 初始化 BLE 控制器 + NimBLE host + BluFi profile 并开始广播。
 * 需在 wifi_manager_init() 之后调用（其已初始化 NVS/WiFi 事件循环）。
 * 配网事件回调会把收到的 STA 凭据经 apply_credentials 落地，并保持 SoftAP
 * 处于 APSTA 模式（忽略手机端切换 WiFi 模式的请求）。
 *
 * 注意：NimBLE 常驻，启用后约占用 40 KB SRAM。本组件由 Kconfig 总开关
 * CONFIG_BLUFI_PROVISIONING_ENABLED 控制编译（关闭时 bt 组件整体不参与构建）。
 */
esp_err_t blufi_provisioning_init(const blufi_provisioning_config_t *config);

#ifdef __cplusplus
}
#endif
