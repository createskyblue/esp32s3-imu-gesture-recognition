#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 简单 BLE echo 示例（RX/TX 双特征，BLE-UART 风格）：
 *   服务 0xAB01
 *     - RX 特征 0xAB02：手机往这里写数据
 *     - TX 特征 0xAB03：设备把收到的数据原样通知回去
 * 用于演示如何在 BLE 上"挂载"自定义 GATT 服务作为额外特征
 *（与 BluFi 配网服务并存）。
 *
 * 需在 ble_host_init() 之前调用（注册 pre_enable 钩子，由 ble_host 统一拉起 host）。
 * 由 CONFIG_BLE_ENABLED 决定是否编译。
 */
esp_err_t ble_echo_init(void);

#ifdef __cplusplus
}
#endif
