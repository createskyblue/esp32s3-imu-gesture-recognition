#include "ble_echo.h"

#include <string.h>

#include "esp_bt_defs.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_log.h"

#define BLE_ECHO_TAG        "BLE_ECHO"
#define BLE_ECHO_APP_ID     0x55u
#define BLE_ECHO_SVC_UUID   0xAB01
#define BLE_ECHO_RX_UUID    0xAB02   /* 手机 → 设备：写数据 */
#define BLE_ECHO_TX_UUID    0xAB03   /* 设备 → 手机：通知数据 */
#define BLE_ECHO_MAX_LEN    64u

static uint16_t s_svc_handle;
static uint16_t s_rx_handle;
static uint16_t s_tx_handle;

static void echo_gatts_cb(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                          esp_ble_gatts_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTS_REG_EVT:
        if (param->reg.status != ESP_GATT_OK) {
            ESP_LOGE(BLE_ECHO_TAG, "register failed: %d", param->reg.status);
            break;
        }
        {
            esp_bt_uuid_t svc_uuid = { .len = ESP_UUID_LEN_16, .uuid.uuid16 = BLE_ECHO_SVC_UUID };
            esp_gatt_srvc_id_t svc_id = {
                .id.uuid = svc_uuid,
                .id.inst_id = 0,
                .is_primary = true,
            };
            /* 服务 + RX(2) + TX(2) + TX CCCD(1) = 6 个 handle */
            esp_ble_gatts_create_service(gatts_if, &svc_id, 6);
        }
        break;
    case ESP_GATTS_CREATE_EVT:
        if (param->create.status != ESP_GATT_OK) {
            ESP_LOGE(BLE_ECHO_TAG, "create service failed: %d", param->create.status);
            break;
        }
        s_svc_handle = param->create.service_handle;
        {
            esp_bt_uuid_t rx_uuid = { .len = ESP_UUID_LEN_16, .uuid.uuid16 = BLE_ECHO_RX_UUID };
            esp_ble_gatts_add_char(s_svc_handle, &rx_uuid, ESP_GATT_PERM_WRITE,
                                   ESP_GATT_CHAR_PROP_BIT_WRITE, NULL, NULL);
        }
        break;
    case ESP_GATTS_ADD_CHAR_EVT:
        if (param->add_char.char_uuid.uuid.uuid16 == BLE_ECHO_RX_UUID) {
            /* 已加 RX，接着加 TX（带通知） */
            s_rx_handle = param->add_char.attr_handle;
            esp_bt_uuid_t tx_uuid = { .len = ESP_UUID_LEN_16, .uuid.uuid16 = BLE_ECHO_TX_UUID };
            esp_ble_gatts_add_char(s_svc_handle, &tx_uuid, ESP_GATT_PERM_READ,
                                   ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY,
                                   NULL, NULL);
        } else {
            /* 已加 TX，补 CCCD 以便手机订阅通知 */
            s_tx_handle = param->add_char.attr_handle;
            esp_bt_uuid_t cccd_uuid = {
                .len = ESP_UUID_LEN_16,
                .uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG,
            };
            esp_ble_gatts_add_char_descr(s_svc_handle, &cccd_uuid,
                                         ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, NULL, NULL);
        }
        break;
    case ESP_GATTS_ADD_CHAR_DESCR_EVT:
        esp_ble_gatts_start_service(s_svc_handle);
        break;
    case ESP_GATTS_START_EVT:
        ESP_LOGI(BLE_ECHO_TAG, "echo service started (rx=0x%04x tx=0x%04x)",
                 s_rx_handle, s_tx_handle);
        break;
    case ESP_GATTS_WRITE_EVT: {
        /* RX 收到数据 → 原样从 TX 通知回去 */
        uint16_t len = param->write.len;
        if (len > BLE_ECHO_MAX_LEN) len = BLE_ECHO_MAX_LEN;
        if (param->write.need_rsp) {
            esp_gatt_rsp_t rsp = {0};
            esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                        param->write.trans_id, ESP_GATT_OK, &rsp);
        }
        esp_ble_gatts_send_indicate(gatts_if, param->write.conn_id, s_tx_handle,
                                    len, param->write.value, false);
        ESP_LOGI(BLE_ECHO_TAG, "RX→TX echoed %u bytes", (unsigned)len);
        break;
    }
    default:
        break;
    }
}

esp_err_t ble_echo_init(void)
{
    esp_ble_gatts_register_callback(echo_gatts_cb);
    esp_ble_gatts_app_register(BLE_ECHO_APP_ID);
    ESP_LOGI(BLE_ECHO_TAG, "init OK (RX 0x%04x / TX 0x%04x)", BLE_ECHO_RX_UUID, BLE_ECHO_TX_UUID);
    return ESP_OK;
}
