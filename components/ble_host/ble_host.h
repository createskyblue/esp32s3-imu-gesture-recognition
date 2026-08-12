#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_HOST_DEVICE_NAME_MAX_BYTES 32u

/**
 * BLE 平台：BLE 控制器 + NimBLE host 的 bring-up 与生命周期。
 *
 * 各 BLE 功能（配网 / echo / 主机...）通过 ble_host_register_pre_enable 与
 * ble_host_register_on_sync 挂自己的钩子，互不依赖：
 *   - pre_enable 钩子在 esp_nimble_enable 之前逐个执行（注册 GATT 服务、
 *     配置 ble_hs_cfg 等 NimBLE 要求 "enable 前" 完成的工作）；
 *   - on_sync 钩子在 host sync 之后执行（如 init profile、启动扫描）。
 */

/** 注册一个 "enable 前" 钩子。name 用于诊断日志，返回非 OK 时 ble_host_init 会按名上报。 */
esp_err_t ble_host_register_pre_enable(const char *name, esp_err_t (*cb)(void));

/** 注册一个 "host sync 后" 钩子。name 用于诊断日志。 */
esp_err_t ble_host_register_on_sync(const char *name, void (*cb)(void));

/** Caller-owned BLE host 策略，由 ble_host_init() 拷贝。 */
typedef struct {
    /** BLE 广播/设备名。 */
    char device_name[BLE_HOST_DEVICE_NAME_MAX_BYTES + 1u];
} ble_host_config_t;

/**
 * 初始化 BLE 控制器 + NimBLE host。需在 wifi_manager_init() 之后调用，
 * 且在所有功能的钩子注册之后（pre_enable 钩子会在此执行）。
 * 由 CONFIG_BLE_ENABLED 控制编译。
 */
esp_err_t ble_host_init(const ble_host_config_t *config);

#ifdef __cplusplus
}
#endif
