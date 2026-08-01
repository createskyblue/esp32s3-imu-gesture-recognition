#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"

#define WIFI_STA_RECONNECT_INITIAL_DELAY_MS 5000u
#define WIFI_STA_RECONNECT_MAX_DELAY_MS     60000u
#define DNS_PORT                            53
#define DNS_MAX_QUERY_LEN                   512

static const char *TAG = "WIFI_MGR";
static const uint8_t DNS_A_ANSWER_TEMPLATE[] = {
    0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01,
    0x00, 0x00, 0x00, 60, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x00,
};

/* ── module state ──────────────────────────────────────────────────────── */
static esp_netif_t     *s_sta_netif;
static esp_netif_t     *s_ap_netif;
static wifi_manager_config_t s_startup_config;
static wifi_manager_credentials_t s_credentials;
static bool              s_sta_connected;
static esp_ip4_addr_t    s_sta_ip;
static esp_timer_handle_t s_reconnect_timer;
static uint32_t           s_reconnect_delay_ms = WIFI_STA_RECONNECT_INITIAL_DELAY_MS;
static SemaphoreHandle_t   s_credentials_mutex;
static bool                s_started;
static wifi_manager_time_synced_cb_t s_time_synced_cb;
static void               *s_time_synced_ctx;

/* ── tiny helpers ──────────────────────────────────────────────────────── */
static void copy_str(char *dest, size_t dest_size, const char *src)
{
    if (dest == NULL || dest_size == 0u) return;
    if (src == NULL) { dest[0] = '\0'; return; }
    snprintf(dest, dest_size, "%s", src);
}

static void ip_to_str(esp_ip4_addr_t ip, char *buf, size_t buf_size)
{
    if (buf != NULL && buf_size > 0u)
        snprintf(buf, buf_size, IPSTR, IP2STR(&ip));
}

static void credentials_lock(void)
{
    if (s_credentials_mutex != NULL) {
        (void)xSemaphoreTake(s_credentials_mutex, portMAX_DELAY);
    }
}

static void credentials_unlock(void)
{
    if (s_credentials_mutex != NULL) {
        (void)xSemaphoreGive(s_credentials_mutex);
    }
}

static void current_credentials_get(wifi_manager_credentials_t *out)
{
    if (out == NULL) return;
    credentials_lock();
    *out = s_credentials;
    credentials_unlock();
}

/* ── caller-provided credentials ───────────────────────────────────────── */
static esp_err_t credentials_copy(wifi_manager_credentials_t *dest,
                                  const wifi_manager_credentials_t *source)
{
    if (dest == NULL) return ESP_ERR_INVALID_ARG;
    *dest = (wifi_manager_credentials_t){0};
    if (source == NULL) return ESP_OK;

    const size_t ssid_len = strnlen(source->sta_ssid, sizeof(source->sta_ssid));
    const size_t password_len = strnlen(source->sta_password,
                                        sizeof(source->sta_password));
    if (ssid_len > WIFI_MANAGER_SSID_MAX_BYTES ||
        password_len > WIFI_MANAGER_PASSWORD_MAX_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(dest->sta_ssid, source->sta_ssid, ssid_len + 1u);
    memcpy(dest->sta_password, source->sta_password, password_len + 1u);
    return ESP_OK;
}

static esp_err_t startup_config_copy(wifi_manager_config_t *dest,
                                     const wifi_manager_config_t *source)
{
    if (dest == NULL || source == NULL) return ESP_ERR_INVALID_ARG;
    *dest = (wifi_manager_config_t){0};

    esp_err_t err = credentials_copy(&dest->sta, &source->sta);
    if (err != ESP_OK) return err;

    const size_t ap_ssid_len = strnlen(source->ap_ssid,
                                       sizeof(source->ap_ssid));
    const size_t ap_password_len = strnlen(source->ap_password,
                                           sizeof(source->ap_password));
    const size_t sntp_server_len = strnlen(source->sntp_server,
                                           sizeof(source->sntp_server));
    if (ap_ssid_len == 0u || ap_ssid_len > WIFI_MANAGER_SSID_MAX_BYTES ||
        ap_password_len > WIFI_MANAGER_PASSWORD_MAX_BYTES ||
        (ap_password_len > 0u && ap_password_len < 8u) ||
        sntp_server_len > WIFI_MANAGER_SNTP_SERVER_MAX_BYTES ||
        source->ap_channel == 0u || source->ap_channel > 14u ||
        source->ap_max_connections == 0u ||
        source->ap_max_connections > 10u) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(dest->ap_ssid, source->ap_ssid, ap_ssid_len + 1u);
    memcpy(dest->ap_password, source->ap_password, ap_password_len + 1u);
    memcpy(dest->sntp_server, source->sntp_server, sntp_server_len + 1u);
    dest->ap_channel = source->ap_channel;
    dest->ap_max_connections = source->ap_max_connections;
    dest->captive_portal_dns_enabled = source->captive_portal_dns_enabled;
    return ESP_OK;
}

/* ── WiFi config builders ──────────────────────────────────────────────── */
static wifi_config_t build_sta_config(const wifi_manager_credentials_t *c)
{
    wifi_config_t cfg = {0};
    if (c != NULL) {
        const size_t ssid_len = strnlen(c->sta_ssid, sizeof(cfg.sta.ssid));
        const size_t password_len = strnlen(c->sta_password,
                                            sizeof(cfg.sta.password));
        memcpy(cfg.sta.ssid, c->sta_ssid, ssid_len);
        memcpy(cfg.sta.password, c->sta_password, password_len);
    }
    return cfg;
}

static wifi_config_t build_ap_config(const wifi_manager_config_t *config)
{
    wifi_config_t cfg = {0};
    const size_t ssid_len = strlen(config->ap_ssid);
    const size_t password_len = strlen(config->ap_password);
    memcpy(cfg.ap.ssid, config->ap_ssid, ssid_len);
    memcpy(cfg.ap.password, config->ap_password, password_len);
    cfg.ap.ssid_len = ssid_len;
    cfg.ap.channel = config->ap_channel;
    cfg.ap.max_connection = config->ap_max_connections;
    cfg.ap.authmode = password_len == 0u
        ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_WPA2_PSK;
    return cfg;
}

/* ── STA connect / reconnect ───────────────────────────────────────────── */
static bool has_credentials(const wifi_manager_credentials_t *credentials)
{
    return credentials != NULL && credentials->sta_ssid[0] != '\0';
}

static void stop_reconnect_timer(void)
{
    if (s_reconnect_timer != NULL && esp_timer_is_active(s_reconnect_timer))
        (void)esp_timer_stop(s_reconnect_timer);
}

static esp_err_t connect_sta_now(const wifi_manager_credentials_t *credentials)
{
    stop_reconnect_timer();
    if (!has_credentials(credentials)) {
        ESP_LOGI(TAG, "STA SSID empty; SoftAP stays available for provisioning");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Connecting STA to SSID %s", credentials->sta_ssid);
    return esp_wifi_connect();
}

static esp_err_t connect_current_sta_now(void)
{
    wifi_manager_credentials_t credentials;
    current_credentials_get(&credentials);
    return connect_sta_now(&credentials);
}

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    wifi_manager_credentials_t credentials;
    current_credentials_get(&credentials);
    if (s_sta_connected || !has_credentials(&credentials)) return;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK)
        ESP_LOGW(TAG, "STA reconnect attempt failed: %s", esp_err_to_name(err));
}

static void schedule_reconnect(void)
{
    wifi_manager_credentials_t credentials;
    current_credentials_get(&credentials);
    if (s_sta_connected || !has_credentials(&credentials) ||
        s_reconnect_timer == NULL) return;
    if (esp_timer_is_active(s_reconnect_timer)) return;

    uint32_t delay_ms = s_reconnect_delay_ms;
    esp_err_t err = esp_timer_start_once(s_reconnect_timer, (uint64_t)delay_ms * 1000ULL);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "STA reconnect scheduled in %u ms", (unsigned)delay_ms);
        if (s_reconnect_delay_ms < WIFI_STA_RECONNECT_MAX_DELAY_MS) {
            s_reconnect_delay_ms *= 2u;
            if (s_reconnect_delay_ms > WIFI_STA_RECONNECT_MAX_DELAY_MS)
                s_reconnect_delay_ms = WIFI_STA_RECONNECT_MAX_DELAY_MS;
        }
    } else if (err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "STA reconnect schedule failed: %s", esp_err_to_name(err));
    }
}

static esp_err_t apply_sta_config(const wifi_manager_credentials_t *credentials)
{
    wifi_config_t cfg = build_sta_config(credentials);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set STA config failed: %s", esp_err_to_name(err));
        return err;
    }
    s_sta_connected = false;
    s_sta_ip.addr = 0u;
    s_reconnect_delay_ms = WIFI_STA_RECONNECT_INITIAL_DELAY_MS;
    (void)esp_wifi_disconnect();
    return connect_sta_now(credentials);
}

static esp_err_t enter_provisioning_mode_locked(void)
{
    const wifi_manager_credentials_t empty_credentials = {0};
    s_credentials = empty_credentials;
    stop_reconnect_timer();
    s_sta_connected = false;
    s_sta_ip.addr = 0u;
    s_reconnect_delay_ms = WIFI_STA_RECONNECT_INITIAL_DELAY_MS;
    (void)esp_wifi_disconnect();

    wifi_config_t cfg = build_sta_config(&empty_credentials);
    return esp_wifi_set_config(WIFI_IF_STA, &cfg);
}

/* ── SNTP ──────────────────────────────────────────────────────────────── */
static void sntp_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    (void)arg; (void)event_base; (void)event_id;
    if (event_data != NULL) {
        time_t now = time(NULL);
        struct tm tm_info;
        localtime_r(&now, &tm_info);
        ESP_LOGI(TAG, "SNTP time synced: %04d-%02d-%02d %02d:%02d:%02d UTC+8",
                 tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
                 tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
        if (s_time_synced_cb != NULL) {
            s_time_synced_cb(s_time_synced_ctx);
        }
    }
}

static void sntp_start(void)
{
    static bool initialized = false;
    if (initialized || s_startup_config.sntp_server[0] == '\0') return;
    initialized = true;

    esp_sntp_config_t config =
        ESP_NETIF_SNTP_DEFAULT_CONFIG(s_startup_config.sntp_server);
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(err));
    else
        ESP_LOGI(TAG, "SNTP started, waiting for time sync...");
}

/* ── WiFi event handler ────────────────────────────────────────────────── */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        s_reconnect_delay_ms = WIFI_STA_RECONNECT_INITIAL_DELAY_MS;
        (void)connect_current_sta_now();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        s_sta_connected = false;
        s_sta_ip.addr = 0u;
        ESP_LOGI(TAG, "WiFi disconnected, reason=%u; backoff reconnect",
                 event != NULL ? (unsigned)event->reason : 0u);
        schedule_reconnect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_sta_connected = true;
        s_sta_ip = event->ip_info.ip;
        s_reconnect_delay_ms = WIFI_STA_RECONNECT_INITIAL_DELAY_MS;
        stop_reconnect_timer();
        ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        sntp_start();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        s_sta_connected = false;
        s_sta_ip.addr = 0u;
        ESP_LOGI(TAG, "WiFi lost IP");
        schedule_reconnect();
    }
}

/* ── DNS hijack (captive portal) ───────────────────────────────────────── */
static void dns_server_task(void *arg)
{
    (void)arg;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(DNS_PORT);
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { ESP_LOGE(TAG, "dns: socket failed"); vTaskDelete(NULL); return; }
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "dns: bind failed"); close(sock); vTaskDelete(NULL); return;
    }
    ESP_LOGI(TAG, "DNS hijack server started on port %u", DNS_PORT);

    uint8_t buf[DNS_MAX_QUERY_LEN];
    while (1) {
        struct sockaddr_in from = {0};
        socklen_t fromlen = sizeof(from);
        int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
        if (len < 12) continue;

        uint8_t response[
            DNS_MAX_QUERY_LEN + sizeof(DNS_A_ANSWER_TEMPLATE)];
        memcpy(response, buf, (size_t)len);
        response[2] |= 0x80; response[3] |= 0x80;
        response[6] = 0x00; response[7] = 0x01;

        size_t answer_off = (size_t)len;
        memcpy(response + answer_off, DNS_A_ANSWER_TEMPLATE,
               sizeof(DNS_A_ANSWER_TEMPLATE));

        esp_netif_ip_info_t ip_info;
        if (s_ap_netif && esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK) {
            uint32_t ip = ip_info.ip.addr;
            memcpy(response + answer_off + sizeof(DNS_A_ANSWER_TEMPLATE) - 4,
                   &ip, 4);
        } else {
            response[answer_off + sizeof(DNS_A_ANSWER_TEMPLATE) - 4] = 192;
            response[answer_off + sizeof(DNS_A_ANSWER_TEMPLATE) - 3] = 168;
            response[answer_off + sizeof(DNS_A_ANSWER_TEMPLATE) - 2] = 4;
            response[answer_off + sizeof(DNS_A_ANSWER_TEMPLATE) - 1] = 1;
        }
        sendto(sock, response,
               answer_off + sizeof(DNS_A_ANSWER_TEMPLATE), 0,
               (struct sockaddr *)&from, fromlen);
    }
}

static esp_err_t dns_start(void)
{
    return xTaskCreate(dns_server_task, "dns_server", 3072, NULL, 5, NULL) == pdPASS
        ? ESP_OK : ESP_ERR_NO_MEM;
}

/* ── public: init ──────────────────────────────────────────────────────── */
esp_err_t wifi_manager_init(const wifi_manager_config_t *config)
{
    wifi_manager_config_t initial_config;
    esp_err_t err = startup_config_copy(&initial_config, config);
    if (err != ESP_OK) return err;

    if (s_credentials_mutex == NULL) {
        s_credentials_mutex = xSemaphoreCreateMutex();
        if (s_credentials_mutex == NULL) return ESP_ERR_NO_MEM;
    }
    credentials_lock();
    if (s_started) {
        credentials_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_credentials = (wifi_manager_credentials_t){0};
    s_startup_config = initial_config;
    credentials_unlock();

    ESP_RETURN_ON_ERROR(nvs_flash_init(), TAG, "NVS init failed");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG,
                        "default event loop creation failed");
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();
    if (s_sta_netif == NULL || s_ap_netif == NULL) return ESP_ERR_NO_MEM;

    if (initial_config.captive_portal_dns_enabled) {
        /* Advertise the SoftAP as DNS server for captive-portal clients. */
        esp_netif_dns_info_t dns_info = {0};
        dns_info.ip.u_addr.ip4.addr = esp_ip4addr_aton("192.168.4.1");
        dns_info.ip.type = ESP_IPADDR_TYPE_V4;
        uint8_t dns_offer_enabled = 1u;
        ESP_RETURN_ON_ERROR(
            esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
                                   ESP_NETIF_DOMAIN_NAME_SERVER,
                                   &dns_offer_enabled,
                                   sizeof(dns_offer_enabled)),
            TAG, "SoftAP DHCP DNS option failed");
        ESP_RETURN_ON_ERROR(
            esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN,
                                   &dns_info),
            TAG, "SoftAP DNS address failed");
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "WiFi driver init failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG,
                        "WiFi RAM storage selection failed");

    const esp_timer_create_args_t timer_args = {
        .callback = reconnect_timer_cb,
        .name = "wifi_reconnect",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_reconnect_timer), TAG,
                        "reconnect timer creation failed");

    esp_event_handler_instance_t inst_any, inst_got_ip;
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &inst_any),
        TAG, "WiFi event handler registration failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
        IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &inst_got_ip),
        TAG, "IP event handler registration failed");
    if (initial_config.sntp_server[0] != '\0') {
        esp_event_handler_instance_t inst_sntp;
        ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
            NETIF_SNTP_EVENT, NETIF_SNTP_TIME_SYNC, &sntp_event_handler, NULL,
            &inst_sntp), TAG, "SNTP event handler registration failed");
    }

    const wifi_manager_credentials_t empty_credentials = {0};
    wifi_config_t sta_cfg = build_sta_config(&empty_credentials);
    wifi_config_t ap_cfg  = build_ap_config(&initial_config);

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG,
                        "APSTA mode selection failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg), TAG,
                        "SoftAP config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg), TAG,
                        "empty STA config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "WiFi start failed");

    credentials_lock();
    s_started = true;
    credentials_unlock();

    if (initial_config.captive_portal_dns_enabled) {
        ESP_RETURN_ON_ERROR(dns_start(), TAG, "DNS task creation failed");
    }

    if (has_credentials(&initial_config.sta)) {
        err = wifi_manager_set_credentials(&initial_config.sta);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "initial STA config rejected; SoftAP remains active: %s",
                     esp_err_to_name(err));
            return err;
        }
    }

    ESP_LOGI(TAG, "WiFi APSTA init done, AP=%s STA=%s",
             initial_config.ap_ssid, initial_config.sta.sta_ssid);
    return ESP_OK;
}

/* ── public: snapshot ──────────────────────────────────────────────────── */
void wifi_manager_get_snapshot(wifi_snapshot_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));

    wifi_manager_credentials_t credentials;
    current_credentials_get(&credentials);

    out->sta_connected = s_sta_connected;
    copy_str(out->sta_ssid, sizeof(out->sta_ssid), credentials.sta_ssid);

    /* Resolve current STA IP (may have changed since event) */
    esp_ip4_addr_t sta_ip = s_sta_ip;
    if (s_sta_netif != NULL && s_sta_connected) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(s_sta_netif, &info) == ESP_OK)
            sta_ip = info.ip;
    }
    ip_to_str(sta_ip, out->sta_ip, sizeof(out->sta_ip));

    if (s_ap_netif != NULL) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(s_ap_netif, &info) == ESP_OK)
            ip_to_str(info.ip, out->ap_ip, sizeof(out->ap_ip));
    }
    if (out->ap_ip[0] == '\0') copy_str(out->ap_ip, sizeof(out->ap_ip), "0.0.0.0");

    out->has_password = credentials.sta_password[0] != '\0';
}

void wifi_manager_get_credentials(wifi_manager_credentials_t *out)
{
    current_credentials_get(out);
}

bool wifi_manager_is_started(void)
{
    credentials_lock();
    const bool started = s_started;
    credentials_unlock();
    return started;
}

/* ── public: update credentials ────────────────────────────────────────── */
esp_err_t wifi_manager_set_credentials(
    const wifi_manager_credentials_t *config)
{
    if (config == NULL) return ESP_ERR_INVALID_ARG;

    wifi_manager_credentials_t credentials;
    esp_err_t err = credentials_copy(&credentials, config);
    if (err != ESP_OK) return err;

    if (s_credentials_mutex == NULL) return ESP_ERR_INVALID_STATE;
    credentials_lock();
    if (!s_started) {
        credentials_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    const wifi_manager_credentials_t previous = s_credentials;
    s_credentials = credentials;
    err = apply_sta_config(&credentials);
    if (err == ESP_OK) {
        credentials_unlock();
        return ESP_OK;
    }

    s_credentials = previous;
    const esp_err_t rollback_err = apply_sta_config(&previous);
    if (rollback_err != ESP_OK) {
        ESP_LOGE(TAG, "restore previous STA config failed: %s",
                 esp_err_to_name(rollback_err));
        const esp_err_t safe_mode_err = enter_provisioning_mode_locked();
        if (safe_mode_err != ESP_OK) {
            ESP_LOGE(TAG, "clear STA driver config failed: %s",
                     esp_err_to_name(safe_mode_err));
        }
    }
    credentials_unlock();
    return err;
}

esp_err_t wifi_manager_enter_provisioning_mode(void)
{
    if (s_credentials_mutex == NULL) return ESP_ERR_INVALID_STATE;
    credentials_lock();
    if (!s_started) {
        credentials_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = enter_provisioning_mode_locked();
    credentials_unlock();
    return err;
}

/* ── public: constants ─────────────────────────────────────────────────── */
const char *wifi_manager_get_ap_ssid(void) { return s_startup_config.ap_ssid; }

const char *wifi_manager_get_ap_password(void)
{
    return s_startup_config.ap_password;
}

esp_err_t wifi_manager_set_time_synced_callback(
    wifi_manager_time_synced_cb_t callback, void *ctx)
{
    credentials_lock();
    s_time_synced_cb = callback;
    s_time_synced_ctx = ctx;
    credentials_unlock();
    return ESP_OK;
}
