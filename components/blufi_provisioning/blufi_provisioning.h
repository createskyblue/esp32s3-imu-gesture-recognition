#pragma once

#include "esp_err.h"
#include "wifi_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Caller-owned BluFi provisioning policy, copied by blufi_provisioning_init(). */
typedef struct {
    /** STA 凭据落地回调：应用凭据 + 原子持久化 + 失败回滚。由应用层提供，
     *  例如 wifi_config_store_apply_credentials()。不可为空。 */
    esp_err_t (*apply_credentials)(const wifi_manager_credentials_t *credentials);
} blufi_provisioning_config_t;

/**
 * 注册 BluFi 配网：向 ble_host 挂载 pre_enable / on_sync 钩子（enable 前配置
 * BluFi GATT，host sync 后 init profile），并注册 WiFi/IP 事件上报。
 * BLE host 的 bring-up 由 ble_host 负责，本组件不再拉起。
 *
 * 需在 ble_host_init() 之前调用（其触发各钩子执行）。
 * 由 CONFIG_BLUFI_PROVISIONING_ENABLED 控制编译。
 */
esp_err_t blufi_provisioning_init(const blufi_provisioning_config_t *config);

#ifdef __cplusplus
}
#endif
