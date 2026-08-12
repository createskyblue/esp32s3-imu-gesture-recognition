#include "ble_host_test.h"

#include <string.h>

#include "esp_bt_defs.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_defs.h"
#include "esp_gattc_api.h"
#include "esp_log.h"

#define BLE_HOST_TAG     "BLE_HOST"
#define BLE_HOST_APP_ID  0x56u

/* 睡眠垫 Nordic UART Service (NUS)：UUID 按 BLE 小端字节序
 * 6E400001-B5A3-F393-E0A9-E50E24DCCA9E → 9E CA DC 24 0E E5 A9 E0 93 F3 A3 B5 01 00 40 6E */
static const uint8_t NUS_SVC_UUID128[16] = {
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E,
};
/* 6E400003-B5A3-F393-E0A9-E50E24DCCA9E (TX/Notify) → ... 03 00 40 6E */
static const uint8_t NUS_TX_UUID128[16] = {
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E,
};

static esp_gatt_if_t s_gattc_if;
static uint16_t s_conn_id;
static uint16_t s_svc_handle;
static uint16_t s_tx_handle;
static esp_bd_addr_t s_remote_bda;
static esp_ble_addr_type_t s_remote_addr_type;

/* 在广播数据里找 128 位服务 UUID（AD type 0x06/0x07） */
static bool adv_has_service_uuid(const uint8_t *adv, uint8_t adv_len,
                                 const uint8_t *uuid128)
{
    uint8_t i = 0;
    while (i + 1 < adv_len) {
        uint8_t len = adv[i];
        if (len == 0) break;
        uint8_t type = adv[i + 1];
        uint8_t dlen = len - 1;
        const uint8_t *data = &adv[i + 2];
        if ((type == 0x06 || type == 0x07) && dlen == 16 &&
            memcmp(data, uuid128, 16) == 0) {
            return true;
        }
        i += len + 1;
    }
    return false;
}

/* 从广播数据里取设备名（AD type 0x08 完整名 / 0x09 短名） */
static void get_dev_name(const uint8_t *adv, uint8_t len, char *out, uint8_t out_size)
{
    out[0] = '\0';
    uint8_t i = 0;
    while (i + 1 < len) {
        uint8_t l = adv[i];
        if (l == 0) break;
        uint8_t type = adv[i + 1];
        uint8_t dlen = l - 1;
        if ((type == 0x08 || type == 0x09) && dlen > 0) {
            uint8_t n = (dlen < out_size - 1) ? dlen : (out_size - 1);
            memcpy(out, &adv[i + 2], n);
            out[n] = '\0';
            return;
        }
        i += l + 1;
    }
}

/* 睡眠状态映射（AGENTS.md 2.5.1，推测） */
static const char *sleep_state_name(uint8_t v)
{
    switch (v) {
    case 0: return "清醒";
    case 1: return "浅睡";
    case 2: return "深睡";
    case 3: return "REM";
    default: return "未知";
    }
}

/* 12 位有符号采样转换：<0x0800 直接为正，>=0x0800 减 0x1000 得负数 */
static int16_t sample_12bit(uint16_t v)
{
    return (v < 0x0800) ? (int16_t)v : (int16_t)(v - 0x1000);
}

/* 直接按 20 字节边界解析数据包（AGENTS.md 2.3~2.6），输出可读结果；
 * 原始字节放到 DEBUG 级，默认日志不刷屏。 */
static void parse_packet(uint8_t *d, uint16_t len)
{
    ESP_LOG_BUFFER_HEX_LEVEL(BLE_HOST_TAG, d, len, ESP_LOG_DEBUG);
    if (len < 20) {
        ESP_LOGW(BLE_HOST_TAG, "非 20 字节包(len=%u)，丢弃", len);
        return;
    }
    switch (d[2]) {
    case 0x09: {
        uint32_t dur_ms = ((uint32_t)d[7] << 24) | ((uint32_t)d[8] << 16) |
                          ((uint32_t)d[9] << 8) | d[10];
        uint16_t evt_s = ((uint16_t)d[11] << 8) | d[12];
        ESP_LOGI(BLE_HOST_TAG,
                 "心率=%u次/分  呼吸=%u次/分  睡眠=%s  记录=%02u:%02u:%02u  事件=%us",
                 d[3], d[4], sleep_state_name(d[5]),
                 (unsigned)(dur_ms / 3600000),
                 (unsigned)((dur_ms % 3600000) / 60000),
                 (unsigned)((dur_ms % 60000) / 1000), evt_s);
        break;
    }
    case 0x10: {
        int16_t s[8];
        for (int i = 0; i < 8; i++) {
            uint16_t raw = ((uint16_t)d[3 + i * 2] << 8) | d[4 + i * 2];
            s[i] = sample_12bit(raw);
        }
        ESP_LOGI(BLE_HOST_TAG, "采样: %d %d %d %d %d %d %d %d",
                 s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]);
        break;
    }
    case 0x50:
        ESP_LOGI(BLE_HOST_TAG, "设备启动");
        break;
    default:
        ESP_LOGI(BLE_HOST_TAG, "未知命令 0x%02x", d[2]);
        break;
    }
}

/* GAP 扫描结果 → 从 blufi 分发器转发进来 */
void ble_host_test_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        esp_ble_gap_start_scanning(0);
        ESP_LOGI(BLE_HOST_TAG, "scanning for sleep mat...");
        break;
    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        if (param->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) break;
        /* 名字可能在广播里，也可能在 scan response 里 */
        char name[32];
        get_dev_name(param->scan_rst.ble_adv, param->scan_rst.adv_data_len,
                     name, sizeof(name));
        if (name[0] == '\0') {
            get_dev_name(param->scan_rst.ble_adv + param->scan_rst.adv_data_len,
                         param->scan_rst.scan_rsp_len, name, sizeof(name));
        }
        const bool has_nus = adv_has_service_uuid(param->scan_rst.ble_adv,
                                                  param->scan_rst.adv_data_len,
                                                  NUS_SVC_UUID128);
        const bool is_sp = (strncmp(name, "SP", 2) == 0);
        ESP_LOGI(BLE_HOST_TAG, "scan: '%s' %02x:%02x:%02x:%02x:%02x:%02x nus=%d",
                 name[0] ? name : "(no-name)",
                 param->scan_rst.bda[0], param->scan_rst.bda[1], param->scan_rst.bda[2],
                 param->scan_rst.bda[3], param->scan_rst.bda[4], param->scan_rst.bda[5], has_nus);
        if (has_nus || is_sp) {
            ESP_LOGI(BLE_HOST_TAG, "target found, connecting...");
            esp_ble_gap_stop_scanning();
            memcpy(s_remote_bda, param->scan_rst.bda, sizeof(s_remote_bda));
            s_remote_addr_type = param->scan_rst.ble_addr_type;
            esp_ble_gattc_open(s_gattc_if, s_remote_bda, s_remote_addr_type, true);
        }
        break;
    }
    default:
        break;
    }
}

static void gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                     esp_ble_gattc_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTC_REG_EVT:
        if (param->reg.status != ESP_GATT_OK) {
            ESP_LOGE(BLE_HOST_TAG, "GATTC register failed: %d", param->reg.status);
            break;
        }
        s_gattc_if = gattc_if;
        {
            esp_ble_scan_params_t scan_params = {
                .scan_type = BLE_SCAN_TYPE_ACTIVE,
                .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
                .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
                .scan_interval = 0x50,
                .scan_window = 0x30,
                .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE,
            };
            esp_ble_gap_set_scan_params(&scan_params);
        }
        break;
    case ESP_GATTC_OPEN_EVT:
        if (param->open.status != ESP_GATT_OK) {
            ESP_LOGE(BLE_HOST_TAG, "open failed: %d", param->open.status);
            break;
        }
        s_conn_id = param->open.conn_id;
        ESP_LOGI(BLE_HOST_TAG, "connected, discovering services");
        esp_ble_gattc_search_service(gattc_if, s_conn_id, NULL);
        break;
    case ESP_GATTC_SEARCH_RES_EVT:
        ESP_LOGI(BLE_HOST_TAG, "  svc found: len=%d start=0x%04x end=0x%04x",
                 param->search_res.srvc_id.uuid.len,
                 param->search_res.start_handle, param->search_res.end_handle);
        ESP_LOG_BUFFER_HEX(BLE_HOST_TAG, param->search_res.srvc_id.uuid.uuid.uuid128,
                           param->search_res.srvc_id.uuid.len);
        if (param->search_res.srvc_id.uuid.len == ESP_UUID_LEN_128 &&
            memcmp(param->search_res.srvc_id.uuid.uuid.uuid128,
                   NUS_SVC_UUID128, 16) == 0) {
            s_svc_handle = param->search_res.start_handle;
        }
        break;
    case ESP_GATTC_SEARCH_CMPL_EVT: {
        if (s_svc_handle == 0) {
            ESP_LOGE(BLE_HOST_TAG, "NUS service not found");
            break;
        }
        esp_bt_uuid_t tx_uuid = { .len = ESP_UUID_LEN_128 };
        memcpy(tx_uuid.uuid.uuid128, NUS_TX_UUID128, 16);
        esp_gattc_char_elem_t chars[8];
        uint16_t count = 8;
        if (esp_ble_gattc_get_char_by_uuid(gattc_if, s_conn_id, s_svc_handle,
                                           0xFFFF, tx_uuid, chars, &count) == ESP_GATT_OK &&
            count > 0) {
            s_tx_handle = chars[0].char_handle;
            ESP_LOGI(BLE_HOST_TAG, "TX char found (0x%04x), subscribing notify", s_tx_handle);
            esp_ble_gattc_register_for_notify(gattc_if, s_remote_bda, s_tx_handle);
        } else {
            ESP_LOGE(BLE_HOST_TAG, "TX char not found");
        }
        break;
    }
    case ESP_GATTC_REG_FOR_NOTIFY_EVT:
        if (param->reg_for_notify.status == ESP_GATT_OK) {
            esp_gattc_descr_elem_t descr;
            uint16_t dcount = 1;
            esp_bt_uuid_t cccd_uuid = { .len = ESP_UUID_LEN_16,
                                        .uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG };
            if (esp_ble_gattc_get_descr_by_char_handle(gattc_if, s_conn_id, s_tx_handle,
                                                       cccd_uuid, &descr, &dcount) == ESP_GATT_OK &&
                dcount > 0) {
                uint8_t value[2] = {0x01, 0x00}; /* 使能通知 */
                esp_ble_gattc_write_char_descr(gattc_if, s_conn_id, descr.handle,
                                               sizeof(value), value,
                                               ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
                ESP_LOGI(BLE_HOST_TAG, "notify subscribed");
            }
        } else {
            ESP_LOGE(BLE_HOST_TAG, "register_for_notify failed: %d", param->reg_for_notify.status);
        }
        break;
    case ESP_GATTC_NOTIFY_EVT:
        parse_packet(param->notify.value, param->notify.value_len);
        break;
    default:
        break;
    }
}

esp_err_t ble_host_test_init(void)
{
    esp_ble_gattc_register_callback(gattc_cb);
    esp_ble_gattc_app_register(BLE_HOST_APP_ID);
    ESP_LOGI(BLE_HOST_TAG, "init OK (BLE central, scanning for sleep mat NUS)");
    return ESP_OK;
}
