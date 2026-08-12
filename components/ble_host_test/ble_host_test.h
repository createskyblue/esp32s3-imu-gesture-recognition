#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * BLE 主机(central)测试(NimBLE)：扫描睡眠垫板子(Nordic UART 服务 6E400001,
 * 设备名 SP*)，连接 → 发现 TX 特征(6E400003) → 订阅 Notify → 打印/解析
 * 固定 20 字节数据包(0x09 心率/呼吸/睡眠、0x10 采样、0x50 启动)。
 *
 * NimBLE 下每个 GAP 操作自带事件回调，不与 BluFi 配网/echo 冲突。
 * 需在 BLE 初始化之后调用（当前由 blufi_provisioning_init 拉起）。
 */
esp_err_t ble_host_test_init(void);

#ifdef __cplusplus
}
#endif
