#pragma once

#include "esp_err.h"
#include "esp_gap_ble_api.h"
#include "wifi_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLUFI_PROVISIONING_DEVICE_NAME_MAX_BYTES 32u

/** 应用自有的 GAP 事件回调（扫描结果、连接状态等）。 */
typedef void (*blufi_provisioning_gap_cb_t)(esp_gap_ble_cb_event_t event,
                                            esp_ble_gap_cb_param_t *param);

/** Caller-owned BluFi provisioning policy, copied by blufi_provisioning_init(). */
typedef struct {
    /** BLE 广播/设备名（随 SoftAP 一起广播，缺省为空串则用 BluFi 默认名）。 */
    char device_name[BLUFI_PROVISIONING_DEVICE_NAME_MAX_BYTES + 1u];
    /** STA 凭据落地回调：应用凭据 + 原子持久化 + 失败回滚。由应用层提供，
     *  例如 wifi_config_store_apply_credentials()。不可为空。 */
    esp_err_t (*apply_credentials)(const wifi_manager_credentials_t *credentials);
    /** 可选：GAP 事件分发器会先喂给官方 BluFi，再转发到这里，供应用做
     *  扫描/连接等 BLE 主机功能。此时不要再自己调用
     *  esp_ble_gap_register_callback()（否则会顶掉分发器）。NULL 表示不转发。 */
    blufi_provisioning_gap_cb_t gap_event_cb;
    /** 可选：自定义广播数据（供应用做 BLE 从机/广播自己的服务）。提供后，
     *  BluFi 不再用自己的广播，而用它广播（需在 app 内保持有效）。NULL 用默认。
     *  注意：若还需要 EspBlufi/微信小程序配网，广播里要带上 BluFi 的服务
     *  UUID（0xFFFF），否则配网 App 扫不到。广播参数沿用 BluFi 默认。 */
    const esp_ble_adv_data_t *adv_data;
} blufi_provisioning_config_t;

/**
 * 与 BLE 主机/从机共存规则（无需改本组件内部）：
 * - 主机（central）：GAP 扫描事件经 gap_event_cb 转发给你；GATTC 用独立回调
 *   esp_ble_gattc_register_callback() 直接注册，不冲突。
 * - 从机（peripheral）：GATT 服务用标准 esp_ble_gatts_register_callback() +
 *   esp_ble_gatts_app_register() 注册（用自己的 app_id），BluFi 的 GATT 走底层
 *   BTA 直连，两者不冲突；广播数据用 adv_data 覆盖。
 * 唯一注意：GAP 与 GATTS 的 esp 级回调槽只能各注册一个，本组件已分别用分发器
 * /直连方式处理，应用不要再重复注册 GAP 回调。
 */

/**
 * 初始化 BLE 控制器 + Bluedroid + BluFi profile 并开始广播。
 * 需在 wifi_manager_init() 之后调用（其已初始化 NVS/WiFi 事件循环）。
 * 配网事件回调会把收到的 STA 凭据经 apply_credentials 落地，并保持 SoftAP
 * 处于 APSTA 模式（忽略手机端切换 WiFi 模式的请求）。
 *
 * 注意：BLE 栈常驻，启用后约占用 60 KB SRAM。本组件由 Kconfig 总开关
 * CONFIG_BLUFI_PROVISIONING_ENABLED 控制编译（关闭时 bt 组件整体不参与构建）。
 */
esp_err_t blufi_provisioning_init(const blufi_provisioning_config_t *config);

/** 当前是否有配网手机连着 BLE（供主机代码判断是否给配网让路）。 */
bool blufi_provisioning_is_session_active(void);

#ifdef __cplusplus
}
#endif
