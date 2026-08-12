#pragma once

#include "esp_err.h"
#include "esp_gap_ble_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/** GAP 事件处理：接到 blufi_provisioning 的分发器(gap_event_cb)，接收扫描结果。 */
void ble_host_test_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);

/**
 * BLE 主机(central)测试：扫描睡眠垫板子(Nordic UART 服务 6E400001)，
 * 连接 → 订阅 TX(6E400003) → 把收到的数据直接打印出来。
 * 需在 BLE 初始化之后调用（当前由 blufi_provisioning_init 拉起）。
 */
esp_err_t ble_host_test_init(void);

#ifdef __cplusplus
}
#endif
