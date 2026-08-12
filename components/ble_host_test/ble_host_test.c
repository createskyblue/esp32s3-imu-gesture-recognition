#include "ble_host_test.h"
#include "ble_host.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

#define BLE_HOST_TAG     "BLE_HOST_TEST"

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

static uint16_t s_conn_handle;
static uint16_t s_svc_start;
static uint16_t s_svc_end;
static uint16_t s_tx_val_handle;
static uint16_t s_cccd_handle;
static bool s_connected;

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

/* 从广播数据取设备名（AD type 0x08 完整名 / 0x09 短名） */
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

/* 直接按 20 字节边界解析数据包（AGENTS.md 2.3~2.6），原始字节放 DEBUG 级 */
static void parse_packet(const uint8_t *d, uint16_t len)
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

static void uuid128_from(const uint8_t bytes[16], ble_uuid128_t *out)
{
    out->u.type = BLE_UUID_TYPE_128;
    memcpy(out->value, bytes, 16);
}

static int write_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                    struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle; (void)error; (void)attr; (void)arg;
    return 0;
}

static int chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                  const struct ble_gatt_chr *chr, void *arg);
static int dsc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                  uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg);

static int svc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                  const struct ble_gatt_svc *service, void *arg)
{
    (void)arg;
    if (error->status != 0 && error->status != BLE_HS_EDONE) {
        ESP_LOGE(BLE_HOST_TAG, "svc disc error: %d", error->status);
        return 0;
    }
    if (service != NULL) {
        /* 按 UUID 发现：记录 NUS 服务 handle 范围 */
        s_svc_start = service->start_handle;
        s_svc_end = service->end_handle;
        return 0;
    }
    if (s_svc_start != 0) {
        ESP_LOGI(BLE_HOST_TAG, "NUS svc found, discovering chars");
        ble_gattc_disc_all_chrs(conn_handle, s_svc_start, s_svc_end, chr_cb, NULL);
    } else {
        ESP_LOGE(BLE_HOST_TAG, "NUS service not found");
    }
    return 0;
}

static int chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                  const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if (error->status != 0 && error->status != BLE_HS_EDONE) {
        ESP_LOGE(BLE_HOST_TAG, "chr disc error: %d", error->status);
        return 0;
    }
    if (chr != NULL) {
        ble_uuid128_t tx_uuid;
        uuid128_from(NUS_TX_UUID128, &tx_uuid);
        if (ble_uuid_cmp(&chr->uuid.u, &tx_uuid.u) == 0) {
            s_tx_val_handle = chr->val_handle;
        }
        return 0;
    }
    if (s_tx_val_handle != 0) {
        ESP_LOGI(BLE_HOST_TAG, "TX char found (0x%04x), discovering CCCD", s_tx_val_handle);
        ble_gattc_disc_all_dscs(conn_handle, s_tx_val_handle, s_svc_end, dsc_cb, NULL);
    } else {
        ESP_LOGE(BLE_HOST_TAG, "TX char not found");
    }
    return 0;
}

static int dsc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                  uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)chr_val_handle; (void)arg;
    if (error->status != 0 && error->status != BLE_HS_EDONE) {
        ESP_LOGE(BLE_HOST_TAG, "dsc disc error: %d", error->status);
        return 0;
    }
    if (dsc != NULL) {
        ble_uuid16_t cccd = BLE_UUID16_INIT(0x2902);
        if (ble_uuid_cmp(&dsc->uuid.u, &cccd.u) == 0) {
            s_cccd_handle = dsc->handle;
        }
        return 0;
    }
    if (s_cccd_handle != 0) {
        uint8_t val[2] = {0x01, 0x00}; /* 使能通知 */
        ble_gattc_write_flat(conn_handle, s_cccd_handle, val, sizeof(val), write_cb, NULL);
        ESP_LOGI(BLE_HOST_TAG, "TX subscribed");
    } else {
        ESP_LOGE(BLE_HOST_TAG, "TX CCCD not found");
    }
    return 0;
}

/* GAP 事件：扫描 → 连接 → 收通知 */
static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        const struct ble_gap_disc_desc *d = &event->disc;
        char name[32];
        get_dev_name(d->data, d->length_data, name, sizeof(name));
        const bool has_nus = adv_has_service_uuid(d->data, d->length_data,
                                                  NUS_SVC_UUID128);
        const bool is_sp = (strncmp(name, "SP", 2) == 0);
        ESP_LOGI(BLE_HOST_TAG, "scan evt=%u '%s' nus=%d",
                 d->event_type, name[0] ? name : "(no-name)", has_nus);
        if (has_nus || is_sp) {
            ESP_LOGI(BLE_HOST_TAG, "target found, connecting...");
            ble_gap_disc_cancel();
            ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &d->addr, 30000, NULL,
                            gap_event_handler, NULL);
        }
        break;
    }
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGE(BLE_HOST_TAG, "connect failed: %d", event->connect.status);
            break;
        }
        s_conn_handle = event->connect.conn_handle;
        s_connected = true;
        ESP_LOGI(BLE_HOST_TAG, "connected, discovering NUS service");
        ble_uuid128_t nus_uuid;
        uuid128_from(NUS_SVC_UUID128, &nus_uuid);
        ble_gattc_disc_svc_by_uuid(s_conn_handle, &nus_uuid.u, svc_cb, NULL);
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_connected = false;
        ESP_LOGI(BLE_HOST_TAG, "disconnected");
        break;
    case BLE_GAP_EVENT_NOTIFY_RX:
        if (event->notify_rx.om != NULL) {
            uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
            uint8_t buf[64];
            if (len > sizeof(buf)) len = sizeof(buf);
            os_mbuf_copydata(event->notify_rx.om, 0, len, buf);
            parse_packet(buf, len);
        }
        break;
    default:
        break;
    }
    return 0;
}

static void host_on_sync(void)
{
    /* host sync 后发扫描（enable 后立即调用会因未 sync 而失败） */
    struct ble_gap_disc_params params = {
        .filter_duplicates = 1,
        .passive = 0,
        .itvl = 0x50,
        .window = 0x30,
        .filter_policy = 0,
        .limited = 0,
    };
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &params,
                          gap_event_handler, NULL);
    if (rc != 0) {
        ESP_LOGE(BLE_HOST_TAG, "scan start failed: %d", rc);
    } else {
        ESP_LOGI(BLE_HOST_TAG, "scanning for sleep mat NUS");
    }
}

esp_err_t ble_host_test_init(void)
{
    ESP_RETURN_ON_ERROR(ble_host_register_on_sync("host_test", host_on_sync),
                        BLE_HOST_TAG, "ble_host on-sync registration failed");
    ESP_LOGI(BLE_HOST_TAG, "init OK (BLE central)");
    return ESP_OK;
}
