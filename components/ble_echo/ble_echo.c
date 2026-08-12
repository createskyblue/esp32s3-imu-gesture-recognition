#include "ble_echo.h"
#include "ble_host.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/util/util.h"

#define BLE_ECHO_TAG        "BLE_ECHO"
#define BLE_ECHO_SVC_UUID   0xAB01
#define BLE_ECHO_RX_UUID    0xAB02   /* 手机 → 设备：写数据 */
#define BLE_ECHO_TX_UUID    0xAB03   /* 设备 → 手机：通知数据 */
#define BLE_ECHO_MAX_LEN    64u

static uint16_t s_tx_attr_handle;

static uint16_t get_tx_handle(void)
{
    if (s_tx_attr_handle == 0) {
        ble_uuid16_t svc_uuid = BLE_UUID16_INIT(BLE_ECHO_SVC_UUID);
        ble_uuid16_t tx_uuid = BLE_UUID16_INIT(BLE_ECHO_TX_UUID);
        uint16_t svc_handle;
        ble_gatts_find_chr(&svc_uuid.u, &tx_uuid.u, &svc_handle, &s_tx_attr_handle);
    }
    return s_tx_attr_handle;
}

static int echo_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle;
    (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        /* RX 收到数据 → 原样从 TX 通知回去 */
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len > BLE_ECHO_MAX_LEN) len = BLE_ECHO_MAX_LEN;
        uint8_t buf[BLE_ECHO_MAX_LEN];
        os_mbuf_copydata(ctxt->om, 0, len, buf);
        ESP_LOGI(BLE_ECHO_TAG, "RX→TX echoed %u bytes", len);
        struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, len);
        if (om != NULL) {
            ble_gatts_notify_custom(conn_handle, get_tx_handle(), om);
        }
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

/* 服务 0xAB01：RX 0xAB02 (write) / TX 0xAB03 (read + notify) */
static const struct ble_gatt_svc_def echo_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(BLE_ECHO_SVC_UUID),
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = BLE_UUID16_DECLARE(BLE_ECHO_RX_UUID),
                .access_cb = echo_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = BLE_UUID16_DECLARE(BLE_ECHO_TX_UUID),
                .access_cb = echo_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 },
        },
    },
    { 0 },
};

static esp_err_t echo_pre_enable(void)
{
    /* NimBLE 要求 GATT 服务表在 host sync 前配好（ble_host enable 前执行） */
    int rc = ble_gatts_count_cfg(echo_svcs);
    if (rc != 0) {
        ESP_LOGE(BLE_ECHO_TAG, "count_cfg failed: %d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(echo_svcs);
    if (rc != 0) {
        ESP_LOGE(BLE_ECHO_TAG, "add_svcs failed: %d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t ble_echo_init(void)
{
    ESP_RETURN_ON_ERROR(ble_host_register_pre_enable(echo_pre_enable), BLE_ECHO_TAG,
                        "ble_host pre-enable registration failed");
    ESP_LOGI(BLE_ECHO_TAG, "init OK (RX 0x%04x / TX 0x%04x)",
             BLE_ECHO_RX_UUID, BLE_ECHO_TX_UUID);
    return ESP_OK;
}
