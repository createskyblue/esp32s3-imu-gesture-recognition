#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "sd_logger.h"

/* ── compile-time WiFi fallback ────────────────────────────────────────── */
#if __has_include("wifi_config.h")
#include "wifi_config.h"
#else
#define WIFI_SSID ""
#define WIFI_PASS ""
#endif

#define WIFI_AP_SSID                        "ESP32S3-Template"
#define WIFI_AP_PASS                        "template1234"
#define WIFI_CONFIG_PATH                    "/littlefs/wifi_config.json"
#define WIFI_SSID_MAX_BYTES                 32u
#define WIFI_PASS_MAX_BYTES                 64u
#define WIFI_STA_RECONNECT_INITIAL_DELAY_MS 5000u
#define WIFI_STA_RECONNECT_MAX_DELAY_MS     60000u
#define WIFI_CONFIG_JSON_BUFFER_BYTES       384u
#define DNS_PORT                            53
#define DNS_MAX_QUERY_LEN                   512

static const char *TAG = "WIFI_MGR";

/* ── types ─────────────────────────────────────────────────────────────── */
typedef struct {
    char ssid[WIFI_SSID_MAX_BYTES + 1u];
    char password[WIFI_PASS_MAX_BYTES + 1u];
    bool loaded_from_file;
} wifi_credentials_t;

/* ── module state ──────────────────────────────────────────────────────── */
static esp_netif_t     *s_sta_netif;
static esp_netif_t     *s_ap_netif;
static wifi_credentials_t s_credentials;
static bool              s_sta_connected;
static esp_ip4_addr_t    s_sta_ip;
static esp_timer_handle_t s_reconnect_timer;
static uint32_t           s_reconnect_delay_ms = WIFI_STA_RECONNECT_INITIAL_DELAY_MS;

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

/* ── credentials persistence ───────────────────────────────────────────── */
static void credentials_set_defaults(wifi_credentials_t *c)
{
    if (c == NULL) return;
    *c = (wifi_credentials_t){0};
    copy_str(c->ssid, sizeof(c->ssid), WIFI_SSID);
    copy_str(c->password, sizeof(c->password), WIFI_PASS);
}

static bool json_string(cJSON *root, const char *key,
                        char *dest, size_t dest_size, bool required)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (item == NULL) return !required;
    if (!cJSON_IsString(item) || item->valuestring == NULL) return false;
    if (strlen(item->valuestring) >= dest_size) return false;
    copy_str(dest, dest_size, item->valuestring);
    return true;
}

static bool config_parse_json(const char *json, wifi_credentials_t *c)
{
    if (json == NULL || c == NULL) return false;
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) return false;

    wifi_credentials_t parsed = {0};
    bool ok = json_string(root, "ssid", parsed.ssid, sizeof(parsed.ssid), true) &&
              parsed.ssid[0] != '\0' &&
              json_string(root, "password", parsed.password, sizeof(parsed.password), false);
    cJSON_Delete(root);
    if (!ok) return false;
    parsed.loaded_from_file = true;
    *c = parsed;
    return true;
}

static void config_load(void)
{
    credentials_set_defaults(&s_credentials);

    FILE *f = fopen(WIFI_CONFIG_PATH, "r");
    if (f == NULL) {
        ESP_LOGI(TAG, "WiFi config not found at %s, using default STA SSID", WIFI_CONFIG_PATH);
        return;
    }
    char json[WIFI_CONFIG_JSON_BUFFER_BYTES];
    size_t n = fread(json, 1u, sizeof(json) - 1u, f);
    fclose(f);
    json[n] = '\0';

    wifi_credentials_t loaded = {0};
    if (!config_parse_json(json, &loaded)) {
        ESP_LOGW(TAG, "WiFi config JSON invalid, using default STA SSID");
        return;
    }
    s_credentials = loaded;
    ESP_LOGI(TAG, "WiFi config loaded from %s for SSID %s", WIFI_CONFIG_PATH, s_credentials.ssid);
}

static esp_err_t config_save(const wifi_credentials_t *c)
{
    if (c == NULL || c->ssid[0] == '\0') return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(root, "ssid", c->ssid);
    cJSON_AddStringToObject(root, "password", c->password);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) return ESP_ERR_NO_MEM;

    FILE *f = fopen(WIFI_CONFIG_PATH, "w");
    if (f == NULL) { cJSON_free(json); return ESP_FAIL; }
    int written = fputs(json, f);
    fclose(f);
    cJSON_free(json);
    return written < 0 ? ESP_FAIL : ESP_OK;
}

/* ── WiFi config builders ──────────────────────────────────────────────── */
static wifi_config_t build_sta_config(const wifi_credentials_t *c)
{
    wifi_config_t cfg = {0};
    if (c != NULL) {
        copy_str((char *)cfg.sta.ssid, sizeof(cfg.sta.ssid), c->ssid);
        copy_str((char *)cfg.sta.password, sizeof(cfg.sta.password), c->password);
    }
    return cfg;
}

static wifi_config_t build_ap_config(void)
{
    wifi_config_t cfg = {0};
    copy_str((char *)cfg.ap.ssid, sizeof(cfg.ap.ssid), WIFI_AP_SSID);
    copy_str((char *)cfg.ap.password, sizeof(cfg.ap.password), WIFI_AP_PASS);
    cfg.ap.ssid_len = strlen(WIFI_AP_SSID);
    cfg.ap.channel = 6u;
    cfg.ap.max_connection = 4u;
    cfg.ap.authmode = strlen(WIFI_AP_PASS) == 0u ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_WPA2_PSK;
    return cfg;
}

/* ── STA connect / reconnect ───────────────────────────────────────────── */
static bool has_credentials(void) { return s_credentials.ssid[0] != '\0'; }

static void stop_reconnect_timer(void)
{
    if (s_reconnect_timer != NULL && esp_timer_is_active(s_reconnect_timer))
        (void)esp_timer_stop(s_reconnect_timer);
}

static esp_err_t connect_sta_now(void)
{
    stop_reconnect_timer();
    if (!has_credentials()) {
        ESP_LOGI(TAG, "STA SSID empty; SoftAP stays available for provisioning");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Connecting STA to SSID %s", s_credentials.ssid);
    return esp_wifi_connect();
}

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    if (s_sta_connected || !has_credentials()) return;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK)
        ESP_LOGW(TAG, "STA reconnect attempt failed: %s", esp_err_to_name(err));
}

static void schedule_reconnect(void)
{
    if (s_sta_connected || !has_credentials() || s_reconnect_timer == NULL) return;
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

static esp_err_t apply_sta_config(void)
{
    wifi_config_t cfg = build_sta_config(&s_credentials);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set STA config failed: %s", esp_err_to_name(err));
        return err;
    }
    s_sta_connected = false;
    s_sta_ip.addr = 0u;
    s_reconnect_delay_ms = WIFI_STA_RECONNECT_INITIAL_DELAY_MS;
    (void)esp_wifi_disconnect();
    return connect_sta_now();
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
        sd_logger_notify_time_synced();
    }
}

static void sntp_start(void)
{
    static bool initialized = false;
    if (initialized) return;
    initialized = true;

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
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
        led_cmd_t blink = { .led = LED_BLUE, .type = LED_CMD_BLINK, .period_ms = 400, .on_ms = 200 };
        led_send_cmd(&blink);
        (void)connect_sta_now();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        s_sta_connected = false;
        s_sta_ip.addr = 0u;
        led_cmd_t blink = { .led = LED_BLUE, .type = LED_CMD_BLINK, .period_ms = 400, .on_ms = 200 };
        led_send_cmd(&blink);
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
        led_cmd_t connected = { .led = LED_BLUE, .type = LED_CMD_BLINK, .period_ms = 3000, .on_ms = 200 };
        led_send_cmd(&connected);
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

        uint8_t response[DNS_MAX_QUERY_LEN];
        memcpy(response, buf, (size_t)len);
        response[2] |= 0x80; response[3] |= 0x80;
        response[6] = 0x00; response[7] = 0x01;

        size_t answer_off = (size_t)len;
        uint8_t answer[] = {
            0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01,
            0x00, 0x00, 0x00, 60, 0x00, 0x04,
            0x00, 0x00, 0x00, 0x00
        };
        memcpy(response + answer_off, answer, sizeof(answer));

        esp_netif_ip_info_t ip_info;
        if (s_ap_netif && esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK) {
            uint32_t ip = ip_info.ip.addr;
            memcpy(response + answer_off + sizeof(answer) - 4, &ip, 4);
        } else {
            response[answer_off + sizeof(answer) - 4] = 192;
            response[answer_off + sizeof(answer) - 3] = 168;
            response[answer_off + sizeof(answer) - 2] = 4;
            response[answer_off + sizeof(answer) - 1] = 1;
        }
        sendto(sock, response, answer_off + sizeof(answer), 0,
               (struct sockaddr *)&from, fromlen);
    }
}

static void dns_start(void)
{
    xTaskCreate(dns_server_task, "dns_server", 3072, NULL, 5, NULL);
}

/* ── public: init ──────────────────────────────────────────────────────── */
esp_err_t wifi_manager_init(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();

    /* DHCP: advertise ESP32 as DNS server for captive portal */
    esp_netif_dns_info_t dns_info = {0};
    dns_info.ip.u_addr.ip4.addr = esp_ip4addr_aton("192.168.4.1");
    dns_info.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
                           ESP_NETIF_DOMAIN_NAME_SERVER,
                           &dns_info, sizeof(dns_info));

    config_load();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    const esp_timer_create_args_t timer_args = {
        .callback = reconnect_timer_cb,
        .name = "wifi_reconnect",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_reconnect_timer));

    esp_event_handler_instance_t inst_any, inst_got_ip, inst_sntp;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &inst_any));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &inst_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        NETIF_SNTP_EVENT, NETIF_SNTP_TIME_SYNC, &sntp_event_handler, NULL, &inst_sntp));

    wifi_config_t sta_cfg = build_sta_config(&s_credentials);
    wifi_config_t ap_cfg  = build_ap_config();

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    dns_start();

    ESP_LOGI(TAG, "WiFi APSTA init done, AP=%s STA=%s", WIFI_AP_SSID, s_credentials.ssid);
    return ESP_OK;
}

/* ── public: snapshot ──────────────────────────────────────────────────── */
void wifi_manager_get_snapshot(wifi_snapshot_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));

    out->sta_connected = s_sta_connected;
    copy_str(out->sta_ssid, sizeof(out->sta_ssid), s_credentials.ssid);

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

    out->config_loaded = s_credentials.loaded_from_file;
    out->has_password  = s_credentials.password[0] != '\0';
}

/* ── public: save credentials ──────────────────────────────────────────── */
esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;

    wifi_credentials_t c = { .loaded_from_file = true };
    copy_str(c.ssid, sizeof(c.ssid), ssid);
    if (password != NULL) copy_str(c.password, sizeof(c.password), password);

    esp_err_t err = config_save(&c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to save WiFi config: %s", esp_err_to_name(err));
        return err;
    }
    s_credentials = c;
    return apply_sta_config();
}

/* ── public: constants ─────────────────────────────────────────────────── */
const char *wifi_manager_get_ap_ssid(void)     { return WIFI_AP_SSID; }
const char *wifi_manager_get_config_path(void)  { return WIFI_CONFIG_PATH; }
