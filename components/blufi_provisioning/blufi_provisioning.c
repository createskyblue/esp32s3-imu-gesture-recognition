#include "blufi_provisioning.h"
#include "blufi_security.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_blufi.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"

#define WIFI_CONNECTION_MAX_RETRY 5u

static const char *TAG = "BLUFI_PROV";

/* ── module state ──────────────────────────────────────────────────────── */
static blufi_provisioning_config_t s_config;
static bool s_ble_connected;
static bool s_sta_is_connecting;
static uint8_t s_sta_bssid[6];
static uint8_t s_sta_ssid[32];
static int  s_sta_ssid_len;
static wifi_manager_credentials_t s_pending_creds;   /* 本次配网会话累积的 STA 凭据 */
static char s_ap_ssid[WIFI_MANAGER_SSID_MAX_BYTES + 1u];
static char s_ap_password[WIFI_MANAGER_PASSWORD_MAX_BYTES + 1u];

static void copy_str(char *dest, size_t dest_size, const char *src)
{
    if (dest == NULL || dest_size == 0u) return;
    if (src == NULL) { dest[0] = '\0'; return; }
    snprintf(dest, dest_size, "%s", src);
}

static int softap_conn_num(void)
{
    wifi_sta_list_t list;
    if (esp_wifi_ap_get_sta_list(&list) == ESP_OK) return (int)list.num;
    return 0;
}

/* 向手机上报当前 STA 连接状态（WiFi 状态以 wifi_manager 快照为准）。 */
static void send_wifi_conn_report(void)
{
    if (!s_ble_connected) return;

    wifi_snapshot_t snap;
    wifi_manager_get_snapshot(&snap);

    esp_blufi_extra_info_t info;
    memset(&info, 0, sizeof(info));
    if (s_sta_ssid_len > 0) {
        info.sta_ssid = s_sta_ssid;
        info.sta_ssid_len = s_sta_ssid_len;
    }
    if (s_sta_bssid[0] != 0u) {
        memcpy(info.sta_bssid, s_sta_bssid, sizeof(s_sta_bssid));
        info.sta_bssid_set = true;
    }

    esp_blufi_sta_conn_state_t state;
    if (snap.sta_connected) {
        state = ESP_BLUFI_STA_CONN_SUCCESS;
    } else if (s_sta_is_connecting) {
        state = ESP_BLUFI_STA_CONNECTING;
        info.sta_max_conn_retry = WIFI_CONNECTION_MAX_RETRY;
        info.sta_max_conn_retry_set = true;
    } else {
        state = ESP_BLUFI_STA_CONN_FAIL;
    }

    esp_blufi_send_wifi_conn_report(WIFI_MODE_APSTA, state,
                                    (uint8_t)softap_conn_num(), &info);
    ESP_LOGI(TAG, "reported WiFi state=%d ssid=%s", (int)state, snap.sta_ssid);
}

static void send_wifi_list(void)
{
    if (!s_ble_connected) {
        esp_wifi_scan_stop();
        return;
    }
    uint16_t ap_count = 0;
    if (esp_wifi_scan_get_ap_num(&ap_count) != ESP_OK || ap_count == 0) {
        esp_wifi_scan_stop();
        esp_blufi_send_error_info(ESP_BLUFI_WIFI_SCAN_FAIL);
        return;
    }
    wifi_ap_record_t *ap_list = (wifi_ap_record_t *)calloc(ap_count, sizeof(wifi_ap_record_t));
    esp_blufi_ap_record_t *blufi_list = (esp_blufi_ap_record_t *)calloc(ap_count, sizeof(esp_blufi_ap_record_t));
    if (ap_list == NULL || blufi_list == NULL) {
        free(ap_list);
        free(blufi_list);
        esp_wifi_scan_stop();
        esp_blufi_send_error_info(ESP_BLUFI_WIFI_SCAN_FAIL);
        return;
    }
    esp_wifi_scan_get_ap_records(&ap_count, ap_list);
    for (uint16_t i = 0; i < ap_count; ++i) {
        blufi_list[i].rssi = ap_list[i].rssi;
        memcpy(blufi_list[i].ssid, ap_list[i].ssid, sizeof(ap_list[i].ssid));
    }
    esp_blufi_send_wifi_list(ap_count, blufi_list);
    esp_wifi_scan_stop();
    free(ap_list);
    free(blufi_list);
}

/* ── WiFi/IP 事件：向手机上报状态 ─────────────────────────────────────── */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    switch (event_id) {
    case WIFI_EVENT_STA_CONNECTED: {
        wifi_event_sta_connected_t *e = (wifi_event_sta_connected_t *)event_data;
        s_sta_is_connecting = false;
        if (e != NULL) {
            memcpy(s_sta_bssid, e->bssid, sizeof(s_sta_bssid));
            memcpy(s_sta_ssid, e->ssid, sizeof(s_sta_ssid));
            s_sta_ssid_len = e->ssid_len;
        }
        break;
    }
    case WIFI_EVENT_STA_DISCONNECTED:
        s_sta_is_connecting = false;
        memset(s_sta_ssid, 0, sizeof(s_sta_ssid));
        memset(s_sta_bssid, 0, sizeof(s_sta_bssid));
        s_sta_ssid_len = 0;
        send_wifi_conn_report();
        break;
    case WIFI_EVENT_AP_START:
        send_wifi_conn_report();
        break;
    case WIFI_EVENT_SCAN_DONE:
        send_wifi_list();
        break;
    default:
        break;
    }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_data;
    if (event_id == IP_EVENT_STA_GOT_IP || event_id == IP_EVENT_STA_LOST_IP) {
        s_sta_is_connecting = false;
        send_wifi_conn_report();
    }
}

/* ── BluFi 事件 ────────────────────────────────────────────────────────── */
/* 启动广播：若应用提供了自定义广播数据则用它的（设置后由 BluFi 的 GAP
 * handler 自动起广播），否则用 BluFi 默认广播。 */
static void blufi_provisioning_start_adv(void)
{
    if (s_config.adv_data != NULL) {
        esp_ble_gap_config_adv_data((esp_ble_adv_data_t *)s_config.adv_data);
    } else {
        esp_blufi_adv_start();
    }
}

static void blufi_event_cb(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param)
{
    switch (event) {
    case ESP_BLUFI_EVENT_INIT_FINISH:
        ESP_LOGI(TAG, "BluFi init finished, advertising as %s", s_config.device_name);
        blufi_provisioning_start_adv();
        break;
    case ESP_BLUFI_EVENT_DEINIT_FINISH:
        ESP_LOGI(TAG, "BluFi deinit finished");
        break;
    case ESP_BLUFI_EVENT_BLE_CONNECT:
        ESP_LOGI(TAG, "BLE connected (provisioning session started)");
        s_ble_connected = true;
        esp_blufi_adv_stop();
        blufi_security_init();
        memset(&s_pending_creds, 0, sizeof(s_pending_creds));
        break;
    case ESP_BLUFI_EVENT_BLE_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnected");
        s_ble_connected = false;
        s_sta_is_connecting = false;
        blufi_security_deinit();
        blufi_provisioning_start_adv();
        break;
    case ESP_BLUFI_EVENT_SET_WIFI_OPMODE:
        /* 保持 APSTA：WiFi 模式由 wifi_manager 统一决定 */
        ESP_LOGI(TAG, "phone requested WiFi mode %d; keeping APSTA",
                 (int)param->wifi_mode.op_mode);
        break;
    case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP: {
        ESP_LOGI(TAG, "phone requests STA connect");
        if (s_pending_creds.sta_ssid[0] == '\0') {
            ESP_LOGW(TAG, "no SSID received yet; ignoring connect request");
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            break;
        }
        s_sta_is_connecting = true;
        const esp_err_t err = s_config.apply_credentials(&s_pending_creds);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "apply credentials failed: %s", esp_err_to_name(err));
            s_sta_is_connecting = false;
            esp_blufi_send_error_info(ESP_BLUFI_MSG_STATE_ERROR);
        }
        break;
    }
    case ESP_BLUFI_EVENT_REQ_DISCONNECT_FROM_AP:
        ESP_LOGI(TAG, "phone requests STA disconnect");
        s_sta_is_connecting = false;
        (void)wifi_manager_enter_provisioning_mode();
        send_wifi_conn_report();
        break;
    case ESP_BLUFI_EVENT_GET_WIFI_STATUS:
        ESP_LOGI(TAG, "phone requests WiFi status");
        send_wifi_conn_report();
        break;
    case ESP_BLUFI_EVENT_REPORT_ERROR:
        BLUFI_ERROR("BluFi report error, error code %d", (int)param->report_error.state);
        esp_blufi_send_error_info(param->report_error.state);
        break;
    case ESP_BLUFI_EVENT_RECV_STA_BSSID:
        /* wifi_manager 凭据模型不含 BSSID，忽略 */
        ESP_LOGI(TAG, "recv STA BSSID (ignored)");
        break;
    case ESP_BLUFI_EVENT_RECV_STA_SSID: {
        uint8_t *ssid = param->sta_ssid.ssid;
        const int len = param->sta_ssid.ssid_len;
        if (ssid == NULL || len < 0 ||
            len >= (int)sizeof(s_pending_creds.sta_ssid)) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            break;
        }
        memcpy(s_pending_creds.sta_ssid, ssid, (size_t)len);
        s_pending_creds.sta_ssid[len] = '\0';
        ESP_LOGI(TAG, "recv STA SSID %s", s_pending_creds.sta_ssid);
        break;
    }
    case ESP_BLUFI_EVENT_RECV_STA_PASSWD: {
        uint8_t *passwd = param->sta_passwd.passwd;
        const int len = param->sta_passwd.passwd_len;
        if (passwd == NULL || len < 0 ||
            len >= (int)sizeof(s_pending_creds.sta_password)) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            break;
        }
        memcpy(s_pending_creds.sta_password, passwd, (size_t)len);
        s_pending_creds.sta_password[len] = '\0';
        ESP_LOGI(TAG, "recv STA password (%d bytes)", len);
        break;
    }
    case ESP_BLUFI_EVENT_RECV_SOFTAP_SSID: {
        uint8_t *ssid = param->softap_ssid.ssid;
        const int len = param->softap_ssid.ssid_len;
        if (ssid == NULL || len < 0 || len >= WIFI_MANAGER_SSID_MAX_BYTES) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            break;
        }
        memcpy(s_ap_ssid, ssid, (size_t)len);
        s_ap_ssid[len] = '\0';
        (void)wifi_manager_set_ap_config(s_ap_ssid, s_ap_password);
        ESP_LOGI(TAG, "recv SoftAP SSID %s", s_ap_ssid);
        break;
    }
    case ESP_BLUFI_EVENT_RECV_SOFTAP_PASSWD: {
        uint8_t *passwd = param->softap_passwd.passwd;
        const int len = param->softap_passwd.passwd_len;
        if (passwd == NULL || len < 0 ||
            len >= WIFI_MANAGER_PASSWORD_MAX_BYTES) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            break;
        }
        memcpy(s_ap_password, passwd, (size_t)len);
        s_ap_password[len] = '\0';
        (void)wifi_manager_set_ap_config(s_ap_ssid, s_ap_password);
        ESP_LOGI(TAG, "recv SoftAP password (%d bytes)", len);
        break;
    }
    case ESP_BLUFI_EVENT_GET_WIFI_LIST: {
        ESP_LOGI(TAG, "phone requests WiFi list; starting scan");
        wifi_scan_config_t scan_cfg = {
            .ssid = NULL,
            .bssid = NULL,
            .channel = 0,
            .show_hidden = false,
            .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        };
        if (esp_wifi_scan_start(&scan_cfg, false) != ESP_OK) {
            esp_blufi_send_error_info(ESP_BLUFI_WIFI_SCAN_FAIL);
        }
        break;
    }
    case ESP_BLUFI_EVENT_RECV_SLAVE_DISCONNECT_BLE:
        ESP_LOGI(TAG, "phone requests BLE disconnect");
        esp_blufi_disconnect();
        break;
    case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA:
        ESP_LOGW(TAG, "recv custom data (%d bytes), ignored",
                 (int)param->custom_data.data_len);
        break;
    default:
        /* username / cert / privkey 等暂不处理 */
        break;
    }
}

/* ── GAP 事件分发器 ───────────────────────────────────────────────────── */
/* BLE 的 GAP 回调是全局单槽。这里注册一个分发器，先喂给官方 BluFi，再转发
 * 给应用提供的 gap_event_cb（扫描/连接等主机功能），两者共存互不顶替。 */
static void blufi_gap_event_dispatcher(esp_gap_ble_cb_event_t event,
                                       esp_ble_gap_cb_param_t *param)
{
    esp_blufi_gap_event_handler(event, param);
    if (s_config.gap_event_cb != NULL) {
        s_config.gap_event_cb(event, param);
    }
}

/* ── public: init ──────────────────────────────────────────────────────── */
esp_err_t blufi_provisioning_init(const blufi_provisioning_config_t *config)
{
    if (config == NULL || config->apply_credentials == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_config = *config;

    /* 1. BT controller（BLE only） */
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_bt_controller_init(&bt_cfg), TAG,
                        "BT controller init failed");
    ESP_RETURN_ON_ERROR(esp_bt_controller_enable(ESP_BT_MODE_BLE), TAG,
                        "BT controller enable failed");

    /* 2. Bluedroid host + 设备名（SoftAP 名带 MAC 后缀，便于辨认） */
    esp_bluedroid_config_t bd_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_bluedroid_init_with_cfg(&bd_cfg), TAG,
                        "Bluedroid init failed");
    ESP_RETURN_ON_ERROR(esp_bluedroid_enable(), TAG,
                        "Bluedroid enable failed");
    if (s_config.device_name[0] != '\0') {
        ESP_RETURN_ON_ERROR(esp_ble_gap_set_device_name(s_config.device_name),
                            TAG, "set BLE device name failed");
    }
    ESP_LOGI(TAG, "BLE host ready, device name: %s",
             s_config.device_name[0] != '\0' ? s_config.device_name : "(default)");

    /* 3. BluFi profile + 回调 + GAP */
    static esp_blufi_callbacks_t callbacks = {
        .event_cb = blufi_event_cb,
        .negotiate_data_handler = blufi_dh_negotiate_data_handler,
        .encrypt_func = blufi_aes_encrypt,
        .decrypt_func = blufi_aes_decrypt,
        .checksum_func = blufi_crc_checksum,
    };
    ESP_RETURN_ON_ERROR(esp_blufi_register_callbacks(&callbacks), TAG,
                        "BluFi callback registration failed");
    ESP_RETURN_ON_ERROR(esp_ble_gap_register_callback(blufi_gap_event_dispatcher),
                        TAG, "BLE GAP callback registration failed");
    ESP_RETURN_ON_ERROR(esp_blufi_profile_init(), TAG, "BluFi profile init failed");

    /* 4. WiFi/IP 事件：向手机上报连接状态 */
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                        &wifi_event_handler, NULL), TAG,
                        "WiFi event registration failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID,
                        &ip_event_handler, NULL), TAG,
                        "IP event registration failed");

    /* 记录当前 SoftAP 身份，供 BluFi 部分字段更新时使用 */
    copy_str(s_ap_ssid, sizeof(s_ap_ssid), wifi_manager_get_ap_ssid());
    copy_str(s_ap_password, sizeof(s_ap_password), wifi_manager_get_ap_password());

    ESP_LOGI(TAG, "BluFi provisioning ready, version %04x (awaiting phone)",
             esp_blufi_get_version());
    return ESP_OK;
}

bool blufi_provisioning_is_session_active(void)
{
    return s_ble_connected;
}
