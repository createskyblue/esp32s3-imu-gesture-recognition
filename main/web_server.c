#include "web_server.h"
#include "file_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_ota_service.h"
#include "esp_service.h"
#include "led_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "source/esp_ota_service_source_http.h"
#include "target/esp_ota_service_target_app.h"
#include "target/esp_ota_service_target_data.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "sd_logger.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

/* ── WiFi & storage config (internal to this module) ─────────────────── */
#if __has_include("wifi_config.h")
#include "wifi_config.h"
#else
#define WIFI_SSID ""
#define WIFI_PASS ""
#endif
#define WIFI_AP_SSID                        "ESP32S3-Template"
#define WIFI_AP_PASS                        "template1234"
#define LITTLEFS_BASE_PATH                  "/littlefs"
#define LITTLEFS_INDEX_PATH                 LITTLEFS_BASE_PATH "/index.html"
#define WIFI_CONFIG_PATH                    LITTLEFS_BASE_PATH "/wifi_config.json"
#define HTTP_FILE_BUFFER_BYTES              1024u
#define HTTP_JSON_BUFFER_BYTES              512u
#define OTA_JSON_BUFFER_BYTES               768u
#define OTA_URL_MAX_BYTES                   256u
#define OTA_TASK_STACK_BYTES                8192u
#define OTA_TASK_PRIORITY                   4u
#define OTA_UPLOAD_BUF_SIZE                 4096u
#define WIFI_CONFIG_JSON_BUFFER_BYTES       384u
#define WIFI_SSID_MAX_BYTES                 32u
#define WIFI_PASS_MAX_BYTES                 64u
#define WIFI_STA_RECONNECT_INITIAL_DELAY_MS 5000u
#define WIFI_STA_RECONNECT_MAX_DELAY_MS     60000u

static const char *TAG = "WEB_SERVER";

typedef struct {
    char ssid[WIFI_SSID_MAX_BYTES + 1u];
    char password[WIFI_PASS_MAX_BYTES + 1u];
    bool loaded_from_file;
} wifi_credentials_t;

typedef enum {
    OTA_UPDATE_IDLE = 0,
    OTA_UPDATE_RUNNING,
    OTA_UPDATE_DONE,
    OTA_UPDATE_FAILED,
} ota_update_phase_t;

typedef struct {
    ota_update_phase_t phase;
    char firmware_url[OTA_URL_MAX_BYTES + 1u];
    char filesystem_url[OTA_URL_MAX_BYTES + 1u];
    char current_label[16];
    char message[128];
    uint32_t item_index;
    uint32_t item_count;
    uint64_t bytes_written;
    uint64_t total_bytes;
    int progress;
    esp_err_t last_error;
    bool reboot_required;
} ota_update_state_t;

/* ── WiFi state (private to this module) ─────────────────────────────── */
static esp_netif_t *s_wifi_sta_netif;
static esp_netif_t *s_wifi_ap_netif;
static wifi_credentials_t s_wifi_credentials;
static bool s_wifi_sta_connected;
static esp_ip4_addr_t s_wifi_sta_ip;
static esp_timer_handle_t s_wifi_reconnect_timer;
static uint32_t s_wifi_reconnect_delay_ms = WIFI_STA_RECONNECT_INITIAL_DELAY_MS;
static SemaphoreHandle_t s_ota_mutex;
static ota_update_state_t s_ota_state = {
    .phase = OTA_UPDATE_IDLE,
    .message = "idle",
};

/* ── HTTP server handle ──────────────────────────────────────────────── */
httpd_handle_t s_http_server;

/* ── helpers ──────────────────────────────────────────────────────────── */
static void copy_string(char *dest, size_t dest_size, const char *src)
{
    if (dest == NULL || dest_size == 0u) {
        return;
    }
    if (src == NULL) {
        dest[0] = '\0';
        return;
    }
    snprintf(dest, dest_size, "%s", src);
}

static void ota_state_lock(void)
{
    if (s_ota_mutex != NULL) {
        xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
    }
}

static void ota_state_unlock(void)
{
    if (s_ota_mutex != NULL) {
        xSemaphoreGive(s_ota_mutex);
    }
}

static void ota_state_set_message(const char *message)
{
    copy_string(s_ota_state.message, sizeof(s_ota_state.message), message);
}

static const char *ota_phase_name(ota_update_phase_t phase)
{
    switch (phase) {
    case OTA_UPDATE_IDLE:
        return "idle";
    case OTA_UPDATE_RUNNING:
        return "running";
    case OTA_UPDATE_DONE:
        return "done";
    case OTA_UPDATE_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}

static bool json_optional_url(cJSON *root, const char *key, char *dest, size_t dest_size)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (item == NULL || cJSON_IsNull(item)) {
        dest[0] = '\0';
        return true;
    }
    if (!cJSON_IsString(item) || item->valuestring == NULL || strlen(item->valuestring) >= dest_size) {
        return false;
    }
    copy_string(dest, dest_size, item->valuestring);
    return true;
}

static void ip_to_string(esp_ip4_addr_t ip, char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0u) {
        return;
    }
    snprintf(buffer, buffer_size, IPSTR, IP2STR(&ip));
}

/* ── WiFi config persistence ─────────────────────────────────────────── */
static void wifi_credentials_set_defaults(wifi_credentials_t *credentials)
{
    if (credentials == NULL) {
        return;
    }
    *credentials = (wifi_credentials_t){0};
    copy_string(credentials->ssid, sizeof(credentials->ssid), WIFI_SSID);
    copy_string(credentials->password, sizeof(credentials->password), WIFI_PASS);
}

static bool wifi_json_string(cJSON *root, const char *key, char *dest, size_t dest_size, bool required)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (item == NULL) {
        return !required;
    }
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }
    if (strlen(item->valuestring) >= dest_size) {
        return false;
    }
    copy_string(dest, dest_size, item->valuestring);
    return true;
}

static bool wifi_config_parse_json(const char *json, wifi_credentials_t *credentials)
{
    if (json == NULL || credentials == NULL) {
        return false;
    }

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return false;
    }

    wifi_credentials_t parsed = {0};
    const bool ok =
        wifi_json_string(root, "ssid", parsed.ssid, sizeof(parsed.ssid), true) &&
        parsed.ssid[0] != '\0' &&
        wifi_json_string(root, "password", parsed.password, sizeof(parsed.password), false);
    cJSON_Delete(root);
    if (!ok) {
        return false;
    }

    parsed.loaded_from_file = true;
    *credentials = parsed;
    return true;
}

static void wifi_config_load(void)
{
    wifi_credentials_set_defaults(&s_wifi_credentials);

    FILE *file = fopen(WIFI_CONFIG_PATH, "r");
    if (file == NULL) {
        ESP_LOGI(TAG, "WiFi config not found at %s, using default STA SSID", WIFI_CONFIG_PATH);
        return;
    }

    char json[WIFI_CONFIG_JSON_BUFFER_BYTES];
    const size_t read_bytes = fread(json, 1u, sizeof(json) - 1u, file);
    fclose(file);
    json[read_bytes] = '\0';

    wifi_credentials_t loaded = {0};
    if (!wifi_config_parse_json(json, &loaded)) {
        ESP_LOGW(TAG, "WiFi config JSON is invalid, using default STA SSID");
        return;
    }
    s_wifi_credentials = loaded;
    ESP_LOGI(TAG, "WiFi config loaded from %s for SSID %s", WIFI_CONFIG_PATH, s_wifi_credentials.ssid);
}

static esp_err_t wifi_config_save(const wifi_credentials_t *credentials)
{
    if (credentials == NULL || credentials->ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "ssid", credentials->ssid);
    cJSON_AddStringToObject(root, "password", credentials->password);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    FILE *file = fopen(WIFI_CONFIG_PATH, "w");
    if (file == NULL) {
        cJSON_free(json);
        return ESP_FAIL;
    }
    const int written = fputs(json, file);
    fclose(file);
    cJSON_free(json);
    return written < 0 ? ESP_FAIL : ESP_OK;
}

/* ── WiFi connect / reconnect ────────────────────────────────────────── */
static wifi_config_t wifi_build_sta_config(const wifi_credentials_t *credentials)
{
    wifi_config_t sta_config = {0};
    if (credentials != NULL) {
        copy_string((char *)sta_config.sta.ssid, sizeof(sta_config.sta.ssid), credentials->ssid);
        copy_string((char *)sta_config.sta.password, sizeof(sta_config.sta.password), credentials->password);
    }
    return sta_config;
}

static wifi_config_t wifi_build_ap_config(void)
{
    wifi_config_t ap_config = {0};
    copy_string((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid), WIFI_AP_SSID);
    copy_string((char *)ap_config.ap.password, sizeof(ap_config.ap.password), WIFI_AP_PASS);
    ap_config.ap.ssid_len = strlen(WIFI_AP_SSID);
    ap_config.ap.channel = 6u;
    ap_config.ap.max_connection = 4u;
    ap_config.ap.authmode = strlen(WIFI_AP_PASS) == 0u ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_WPA2_PSK;
    return ap_config;
}

static bool wifi_sta_has_credentials(void)
{
    return s_wifi_credentials.ssid[0] != '\0';
}

static void wifi_stop_sta_reconnect_timer(void)
{
    if (s_wifi_reconnect_timer != NULL && esp_timer_is_active(s_wifi_reconnect_timer)) {
        (void)esp_timer_stop(s_wifi_reconnect_timer);
    }
}

static esp_err_t wifi_connect_sta_now(void)
{
    wifi_stop_sta_reconnect_timer();
    if (!wifi_sta_has_credentials()) {
        ESP_LOGI(TAG, "STA SSID is empty; SoftAP stays available for provisioning");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Connecting STA to SSID %s", s_wifi_credentials.ssid);
    return esp_wifi_connect();
}

static void wifi_reconnect_timer_callback(void *arg)
{
    (void)arg;
    if (s_wifi_sta_connected || !wifi_sta_has_credentials()) {
        return;
    }

    const esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "STA reconnect attempt failed to start: %s", esp_err_to_name(err));
    }
}

static void wifi_schedule_sta_reconnect(void)
{
    if (s_wifi_sta_connected || !wifi_sta_has_credentials() || s_wifi_reconnect_timer == NULL) {
        return;
    }
    if (esp_timer_is_active(s_wifi_reconnect_timer)) {
        return;
    }

    const uint32_t delay_ms = s_wifi_reconnect_delay_ms;
    const esp_err_t err = esp_timer_start_once(s_wifi_reconnect_timer, (uint64_t)delay_ms * 1000ULL);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "STA reconnect scheduled in %u ms", (unsigned)delay_ms);
        if (s_wifi_reconnect_delay_ms < WIFI_STA_RECONNECT_MAX_DELAY_MS) {
            s_wifi_reconnect_delay_ms *= 2u;
            if (s_wifi_reconnect_delay_ms > WIFI_STA_RECONNECT_MAX_DELAY_MS) {
                s_wifi_reconnect_delay_ms = WIFI_STA_RECONNECT_MAX_DELAY_MS;
            }
        }
    } else if (err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "STA reconnect schedule failed: %s", esp_err_to_name(err));
    }
}

static esp_err_t wifi_apply_sta_config(void)
{
    wifi_config_t sta_config = wifi_build_sta_config(&s_wifi_credentials);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set STA config failed: %s", esp_err_to_name(err));
        return err;
    }
    s_wifi_sta_connected = false;
    s_wifi_sta_ip.addr = 0u;
    s_wifi_reconnect_delay_ms = WIFI_STA_RECONNECT_INITIAL_DELAY_MS;
    (void)esp_wifi_disconnect();
    return wifi_connect_sta_now();
}

/* ── SNTP time sync ───────────────────────────────────────────────── */
static void sntp_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;
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

static void sntp_start_sync(void)
{
    static bool sntp_initialized = false;
    if (sntp_initialized) {
        return;
    }
    sntp_initialized = true;

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "SNTP started, waiting for time sync...");
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        s_wifi_reconnect_delay_ms = WIFI_STA_RECONNECT_INITIAL_DELAY_MS;
        led_cmd_t blink = { .led = LED_BLUE, .type = LED_CMD_BLINK, .period_ms = 400, .on_ms = 200 };
        led_send_cmd(&blink);
        (void)wifi_connect_sta_now();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        s_wifi_sta_connected = false;
        s_wifi_sta_ip.addr = 0u;
        led_cmd_t blink = { .led = LED_BLUE, .type = LED_CMD_BLINK, .period_ms = 400, .on_ms = 200 };
        led_send_cmd(&blink);
        ESP_LOGI(TAG,
                 "WiFi disconnected, reason=%u; reconnect will use backoff",
                 event != NULL ? (unsigned)event->reason : 0u);
        wifi_schedule_sta_reconnect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_wifi_sta_connected = true;
        s_wifi_sta_ip = event->ip_info.ip;
        s_wifi_reconnect_delay_ms = WIFI_STA_RECONNECT_INITIAL_DELAY_MS;
        wifi_stop_sta_reconnect_timer();
        ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        led_cmd_t connected = { .led = LED_BLUE, .type = LED_CMD_BLINK, .period_ms = 3000, .on_ms = 200 };
        led_send_cmd(&connected);
        sntp_start_sync();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        s_wifi_sta_connected = false;
        s_wifi_sta_ip.addr = 0u;
        ESP_LOGI(TAG, "WiFi lost IP");
        wifi_schedule_sta_reconnect();
    }
}

/* ── DNS hijack server ───────────────────────────────────────────────── */
#define DNS_PORT 53
#define DNS_MAX_QUERY_LEN 512

static void dns_server_task(void *arg)
{
    (void)arg;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(DNS_PORT);
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        ESP_LOGE(TAG, "dns: socket create failed");
        vTaskDelete(NULL);
        return;
    }
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "dns: bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "dns hijack server started on port %u", DNS_PORT);
    uint8_t buf[DNS_MAX_QUERY_LEN];
    while (1) {
        struct sockaddr_in from = {0};
        socklen_t fromlen = sizeof(from);
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                           (struct sockaddr *)&from, &fromlen);
        if (len < 12) continue;

        uint8_t response[DNS_MAX_QUERY_LEN];
        memcpy(response, buf, (size_t)len);
        response[2] |= 0x80;
        response[3] |= 0x80;
        response[6] = 0x00;
        response[7] = 0x01;

        size_t answer_off = (size_t)len;
        uint8_t answer[] = {
            0xC0, 0x0C,
            0x00, 0x01,
            0x00, 0x01,
            0x00, 0x00, 0x00, 60,
            0x00, 0x04,
            0x00, 0x00, 0x00, 0x00
        };
        memcpy(response + answer_off, answer, sizeof(answer));

        esp_netif_ip_info_t ip_info;
        if (s_wifi_ap_netif && esp_netif_get_ip_info(s_wifi_ap_netif, &ip_info) == ESP_OK) {
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

static void dns_server_start(void)
{
    xTaskCreate(dns_server_task, "dns_server", 3072, NULL, 5, NULL);
}

static void wifi_init(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_wifi_sta_netif = esp_netif_create_default_wifi_sta();
    s_wifi_ap_netif = esp_netif_create_default_wifi_ap();

    /* DHCP: advertise ESP32 as DNS server for captive portal */
    esp_netif_dns_info_t dns_info = {0};
    dns_info.ip.u_addr.ip4.addr = esp_ip4addr_aton("192.168.4.1");
    dns_info.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_dhcps_option(s_wifi_ap_netif, ESP_NETIF_OP_SET,
                           ESP_NETIF_DOMAIN_NAME_SERVER,
                           &dns_info, sizeof(dns_info));

    wifi_config_load();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    const esp_timer_create_args_t reconnect_timer_args = {
        .callback = wifi_reconnect_timer_callback,
        .name = "wifi_reconnect",
    };
    ESP_ERROR_CHECK(esp_timer_create(&reconnect_timer_args, &s_wifi_reconnect_timer));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    esp_event_handler_instance_t instance_sntp;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(NETIF_SNTP_EVENT,
                                                        NETIF_SNTP_TIME_SYNC,
                                                        &sntp_event_handler,
                                                        NULL,
                                                        &instance_sntp));

    wifi_config_t sta_config = wifi_build_sta_config(&s_wifi_credentials);
    wifi_config_t ap_config = wifi_build_ap_config();

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    dns_server_start();

    ESP_LOGI(TAG, "WiFi APSTA init finished, AP=%s STA=%s", WIFI_AP_SSID, s_wifi_credentials.ssid);
}

/* ── LittleFS ────────────────────────────────────────────────────────── */
static esp_err_t littlefs_init(void)
{
    const esp_vfs_littlefs_conf_t conf = {
        .base_path = LITTLEFS_BASE_PATH,
        .partition_label = "storage",
        .format_if_mount_failed = false,
        .dont_mount = false,
    };

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t total = 0u;
    size_t used = 0u;
    err = esp_littlefs_info(conf.partition_label, &total, &used);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LittleFS info failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "LittleFS mounted: total=%u used=%u", (unsigned)total, (unsigned)used);
    }
    return ESP_OK;
}

/* ── HTTP handlers ───────────────────────────────────────────────────── */
static esp_err_t root_handler(httpd_req_t *req)
{
    FILE *file = fopen(LITTLEFS_INDEX_PATH, "r");
    if (file == NULL) {
        ESP_LOGE(TAG, "failed to open %s", LITTLEFS_INDEX_PATH);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "index.html not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");

    char buffer[HTTP_FILE_BUFFER_BYTES];
    size_t read_bytes;
    esp_err_t err = ESP_OK;
    while ((read_bytes = fread(buffer, 1u, sizeof(buffer), file)) > 0u) {
        err = httpd_resp_send_chunk(req, buffer, read_bytes);
        if (err != ESP_OK) {
            break;
        }
    }

    fclose(file);
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
}

/* Serve any file from LittleFS: /foo.html → /littlefs/foo.html */
static esp_err_t littlefs_static_handler(httpd_req_t *req)
{
    char path[576];
    snprintf(path, sizeof(path), "%s%s", LITTLEFS_BASE_PATH, req->uri);

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        /* Redirect unknown paths to / for captive portal detection */
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    /* Guess content type from extension */
    const char *type = "application/octet-stream";
    const char *ext = strrchr(req->uri, '.');
    if (ext) {
        if (strcasecmp(ext, ".html") == 0) type = "text/html; charset=utf-8";
        else if (strcasecmp(ext, ".js") == 0) type = "application/javascript";
        else if (strcasecmp(ext, ".css") == 0) type = "text/css";
        else if (strcasecmp(ext, ".json") == 0) type = "application/json";
        else if (strcasecmp(ext, ".svg") == 0) type = "image/svg+xml";
        else if (strcasecmp(ext, ".png") == 0) type = "image/png";
        else if (strcasecmp(ext, ".ico") == 0) type = "image/x-icon";
    }
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");

    char buffer[HTTP_FILE_BUFFER_BYTES];
    size_t read_bytes;
    esp_err_t err = ESP_OK;
    while ((read_bytes = fread(buffer, 1u, sizeof(buffer), file)) > 0u) {
        err = httpd_resp_send_chunk(req, buffer, read_bytes);
        if (err != ESP_OK) break;
    }
    fclose(file);
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
}

static void blue_data_blink(void)
{
    const led_cmd_t cmd = { .led = LED_BLUE, .type = LED_CMD_ONESHOT, .on_ms = 100 };
    led_send_cmd(&cmd);
}

esp_err_t send_json_text(httpd_req_t *req, const char *json)
{
    blue_data_blink();
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    return httpd_resp_sendstr(req, json != NULL ? json : "{}");
}

esp_err_t send_json_object(httpd_req_t *req, cJSON *root)
{
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json allocation failed");
        return ESP_FAIL;
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json allocation failed");
        return ESP_FAIL;
    }
    esp_err_t err = send_json_text(req, json);
    cJSON_free(json);
    return err;
}

static esp_err_t network_json_handler(httpd_req_t *req)
{
    char sta_ip[16];
    char ap_ip[16] = "0.0.0.0";
    esp_ip4_addr_t current_sta_ip = s_wifi_sta_ip;
    if (s_wifi_sta_netif != NULL && s_wifi_sta_connected) {
        esp_netif_ip_info_t sta_info;
        if (esp_netif_get_ip_info(s_wifi_sta_netif, &sta_info) == ESP_OK) {
            current_sta_ip = sta_info.ip;
        }
    }
    ip_to_string(current_sta_ip, sta_ip, sizeof(sta_ip));

    if (s_wifi_ap_netif != NULL) {
        esp_netif_ip_info_t ap_info;
        if (esp_netif_get_ip_info(s_wifi_ap_netif, &ap_info) == ESP_OK) {
            ip_to_string(ap_info.ip, ap_ip, sizeof(ap_ip));
        }
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json allocation failed");
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(root, "sta_connected", s_wifi_sta_connected);
    cJSON_AddStringToObject(root, "sta_ssid", s_wifi_credentials.ssid);
    cJSON_AddStringToObject(root, "sta_ip", s_wifi_sta_connected ? sta_ip : "0.0.0.0");
    cJSON_AddStringToObject(root, "ap_ssid", WIFI_AP_SSID);
    cJSON_AddStringToObject(root, "ap_ip", ap_ip);
    cJSON_AddStringToObject(root, "config_path", WIFI_CONFIG_PATH);
    cJSON_AddBoolToObject(root, "config_loaded", s_wifi_credentials.loaded_from_file);
    const esp_app_desc_t *app_desc = esp_app_get_description();
    char build_ts[32];
    snprintf(build_ts, sizeof(build_ts), "%s %s", app_desc->date, app_desc->time);
    cJSON_AddStringToObject(root, "app_build_id", "esp32s3-template-v1");
    cJSON_AddStringToObject(root, "firmware_sha256", esp_app_get_elf_sha256_str());
    cJSON_AddStringToObject(root, "build_timestamp", build_ts);
    cJSON_AddStringToObject(root, "idf_version", app_desc->idf_ver);
    return send_json_object(req, root);
}

static esp_err_t wifi_config_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json allocation failed");
        return ESP_FAIL;
    }
    cJSON_AddStringToObject(root, "ssid", s_wifi_credentials.ssid);
    cJSON_AddBoolToObject(root, "has_password", s_wifi_credentials.password[0] != '\0');
    cJSON_AddStringToObject(root, "path", WIFI_CONFIG_PATH);
    cJSON_AddBoolToObject(root, "loaded_from_file", s_wifi_credentials.loaded_from_file);
    return send_json_object(req, root);
}

esp_err_t receive_json_body(httpd_req_t *req, char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0u || req->content_len >= buffer_size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json body too large");
        return ESP_FAIL;
    }

    size_t received = 0u;
    while (received < req->content_len) {
        const int ret = httpd_req_recv(req,
                                      buffer + received,
                                      req->content_len - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed to receive body");
            return ESP_FAIL;
        }
        received += (size_t)ret;
    }
    buffer[received] = '\0';
    return ESP_OK;
}

static esp_err_t wifi_config_post_handler(httpd_req_t *req)
{
    char body[HTTP_JSON_BUFFER_BYTES];
    if (receive_json_body(req, body, sizeof(body)) != ESP_OK) {
        return ESP_FAIL;
    }

    wifi_credentials_t credentials = {0};
    if (!wifi_config_parse_json(body, &credentials)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected JSON with non-empty ssid and optional password");
        return ESP_FAIL;
    }

    esp_err_t err = wifi_config_save(&credentials);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to save WiFi config: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed to save wifi config");
        return ESP_FAIL;
    }

    credentials.loaded_from_file = true;
    s_wifi_credentials = credentials;
    err = wifi_apply_sta_config();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "saved WiFi config but reconnect failed: %s", esp_err_to_name(err));
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json allocation failed");
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "ssid", s_wifi_credentials.ssid);
    cJSON_AddStringToObject(root, "path", WIFI_CONFIG_PATH);
    cJSON_AddStringToObject(root, "message", "saved; reconnecting STA");
    return send_json_object(req, root);
}

static void ota_service_event_handler(const adf_event_t *event, void *ctx)
{
    (void)ctx;
    if (event == NULL || event->payload == NULL || event->payload_len < sizeof(esp_ota_service_event_t)) {
        return;
    }

    const esp_ota_service_event_t *ota_event = (const esp_ota_service_event_t *)event->payload;
    ota_state_lock();
    switch (ota_event->id) {
    case ESP_OTA_SERVICE_EVT_SESSION_BEGIN:
        s_ota_state.phase = OTA_UPDATE_RUNNING;
        s_ota_state.progress = 1;
        ota_state_set_message("OTA session started");
        break;
    case ESP_OTA_SERVICE_EVT_ITEM_BEGIN:
        s_ota_state.item_index = ota_event->item_index + 1u;
        copy_string(s_ota_state.current_label,
                    sizeof(s_ota_state.current_label),
                    ota_event->item_label != NULL ? ota_event->item_label : "item");
        s_ota_state.bytes_written = 0u;
        s_ota_state.total_bytes = 0u;
        ota_state_set_message("downloading");
        break;
    case ESP_OTA_SERVICE_EVT_ITEM_PROGRESS:
        s_ota_state.bytes_written = ota_event->progress.bytes_written;
        s_ota_state.total_bytes = ota_event->progress.total_bytes;
        if (ota_event->progress.total_bytes > 0u) {
            const uint32_t item_percent =
                (uint32_t)((ota_event->progress.bytes_written * 100u) / ota_event->progress.total_bytes);
            s_ota_state.progress =
                (int)(((s_ota_state.item_index - 1u) * 100u + item_percent) / s_ota_state.item_count);
        }
        ota_state_set_message("writing flash");
        break;
    case ESP_OTA_SERVICE_EVT_ITEM_END:
        s_ota_state.last_error = ota_event->error;
        if (ota_event->error != ESP_OK) {
            s_ota_state.phase = OTA_UPDATE_FAILED;
            s_ota_state.progress = 100;
            ota_state_set_message(esp_err_to_name(ota_event->error));
        }
        break;
    case ESP_OTA_SERVICE_EVT_SESSION_END:
        if (ota_event->session_end.aborted || ota_event->session_end.failed_count > 0u) {
            s_ota_state.phase = OTA_UPDATE_FAILED;
            ota_state_set_message("OTA failed");
        } else {
            s_ota_state.phase = OTA_UPDATE_DONE;
            s_ota_state.progress = 100;
            s_ota_state.reboot_required =
                s_ota_state.firmware_url[0] != '\0' || s_ota_state.filesystem_url[0] != '\0';
            ota_state_set_message(s_ota_state.reboot_required ? "done; reboot to apply update" : "done");
        }
        break;
    default:
        break;
    }
    ota_state_unlock();
}

static esp_err_t ota_prepare_item(esp_ota_upgrade_item_t *item,
                                  const char *label,
                                  const char *uri,
                                  bool filesystem)
{
    (void)label;
    if (item == NULL || uri == NULL || uri[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_ota_service_source_t *source = NULL;
    esp_ota_service_target_t *target = NULL;
    esp_err_t err = esp_ota_service_source_http_create(NULL, &source);
    if (err != ESP_OK) {
        return err;
    }

    if (filesystem) {
        err = esp_ota_service_target_data_create(NULL, &target);
        item->partition_label = "storage";
        item->resumable = false;
    } else {
        esp_ota_service_target_app_cfg_t app_cfg = {
            .bulk_flash_erase = true,
        };
        err = esp_ota_service_target_app_create(&app_cfg, &target);
        item->resumable = true;
    }
    if (err != ESP_OK) {
        if (source != NULL && source->destroy != NULL) {
            source->destroy(source);
        }
        return err;
    }

    item->uri = uri;
    item->source = source;
    item->target = target;
    item->skip_on_fail = false;
    return ESP_OK;
}

static void ota_update_task(void *arg)
{
    (void)arg;
    char firmware_url[OTA_URL_MAX_BYTES + 1u];
    char filesystem_url[OTA_URL_MAX_BYTES + 1u];

    ota_state_lock();
    copy_string(firmware_url, sizeof(firmware_url), s_ota_state.firmware_url);
    copy_string(filesystem_url, sizeof(filesystem_url), s_ota_state.filesystem_url);
    s_ota_state.phase = OTA_UPDATE_RUNNING;
    s_ota_state.progress = 1;
    s_ota_state.last_error = ESP_OK;
    ota_state_set_message("starting OTA");
    ota_state_unlock();

    esp_err_t err = ESP_OK;
    esp_ota_service_cfg_t cfg = ESP_OTA_SERVICE_CFG_DEFAULT();
    cfg.worker_task.stack_size = OTA_TASK_STACK_BYTES;
    cfg.worker_task.priority = OTA_TASK_PRIORITY;
    esp_ota_service_t *service = NULL;
    err = esp_ota_service_create(&cfg, &service);
    if (err != ESP_OK) {
        goto done;
    }

    esp_ota_upgrade_item_t items[2] = {0};
    int item_count = 0;
    if (firmware_url[0] != '\0') {
        err = ota_prepare_item(&items[item_count++], "firmware", firmware_url, false);
        if (err != ESP_OK) {
            goto destroy;
        }
    }
    if (filesystem_url[0] != '\0') {
        err = ota_prepare_item(&items[item_count++], "filesystem", filesystem_url, true);
        if (err != ESP_OK) {
            goto destroy;
        }
    }

    ota_state_lock();
    s_ota_state.item_count = (uint32_t)item_count;
    ota_state_unlock();

    adf_event_subscribe_info_t sub = ADF_EVENT_SUBSCRIBE_INFO_DEFAULT();
    sub.handler = ota_service_event_handler;
    err = esp_service_event_subscribe((esp_service_t *)service, &sub);
    if (err != ESP_OK) {
        goto destroy;
    }

    err = esp_ota_service_set_upgrade_list(service, items, item_count);
    if (err == ESP_OK) {
        err = esp_service_start((esp_service_t *)service);
    }
    if (err == ESP_OK) {
        while (true) {
            ota_state_lock();
            const bool finished = s_ota_state.phase == OTA_UPDATE_DONE ||
                                  s_ota_state.phase == OTA_UPDATE_FAILED;
            ota_state_unlock();
            if (finished) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

destroy:
    esp_ota_service_destroy(service);

done:
    if (err != ESP_OK) {
        ota_state_lock();
        s_ota_state.phase = OTA_UPDATE_FAILED;
        s_ota_state.progress = 100;
        s_ota_state.last_error = err;
        ota_state_set_message(esp_err_to_name(err));
        ota_state_unlock();
    }
    vTaskDelete(NULL);
}

static bool ota_parse_request(const char *body, char *firmware_url, char *filesystem_url)
{
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return false;
    }
    const bool ok =
        json_optional_url(root, "firmware_url", firmware_url, OTA_URL_MAX_BYTES + 1u) &&
        json_optional_url(root, "filesystem_url", filesystem_url, OTA_URL_MAX_BYTES + 1u) &&
        (firmware_url[0] != '\0' || filesystem_url[0] != '\0');
    cJSON_Delete(root);
    return ok;
}

static esp_err_t ota_start_handler(httpd_req_t *req)
{
    char body[OTA_JSON_BUFFER_BYTES];
    if (receive_json_body(req, body, sizeof(body)) != ESP_OK) {
        return ESP_FAIL;
    }

    char firmware_url[OTA_URL_MAX_BYTES + 1u] = {0};
    char filesystem_url[OTA_URL_MAX_BYTES + 1u] = {0};
    if (!ota_parse_request(body, firmware_url, filesystem_url)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected firmware_url or filesystem_url");
        return ESP_FAIL;
    }

    ota_state_lock();
    const bool busy = s_ota_state.phase == OTA_UPDATE_RUNNING;
    if (!busy) {
        s_ota_state = (ota_update_state_t){
            .phase = OTA_UPDATE_RUNNING,
            .progress = 0,
            .item_count = (firmware_url[0] != '\0' ? 1u : 0u) + (filesystem_url[0] != '\0' ? 1u : 0u),
            .last_error = ESP_OK,
        };
        copy_string(s_ota_state.firmware_url, sizeof(s_ota_state.firmware_url), firmware_url);
        copy_string(s_ota_state.filesystem_url, sizeof(s_ota_state.filesystem_url), filesystem_url);
        ota_state_set_message("queued");
    }
    ota_state_unlock();

    if (busy) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA already running");
        return ESP_FAIL;
    }

    BaseType_t created = xTaskCreate(ota_update_task,
                                     "ota_update",
                                     OTA_TASK_STACK_BYTES,
                                     NULL,
                                     OTA_TASK_PRIORITY,
                                     NULL);
    if (created != pdPASS) {
        ota_state_lock();
        s_ota_state.phase = OTA_UPDATE_FAILED;
        s_ota_state.last_error = ESP_ERR_NO_MEM;
        ota_state_set_message("failed to create OTA task");
        ota_state_unlock();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed to create OTA task");
        return ESP_FAIL;
    }

    return send_json_text(req, "{\"ok\":true,\"message\":\"OTA started\"}");
}

static esp_err_t ota_status_handler(httpd_req_t *req)
{
    ota_state_lock();
    const ota_update_state_t state = s_ota_state;
    ota_state_unlock();

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json allocation failed");
        return ESP_FAIL;
    }
    cJSON_AddStringToObject(root, "phase", ota_phase_name(state.phase));
    cJSON_AddStringToObject(root, "message", state.message);
    cJSON_AddStringToObject(root, "current_label", state.current_label);
    cJSON_AddNumberToObject(root, "progress", state.progress);
    cJSON_AddNumberToObject(root, "item_index", state.item_index);
    cJSON_AddNumberToObject(root, "item_count", state.item_count);
    cJSON_AddNumberToObject(root, "bytes_written", (double)state.bytes_written);
    cJSON_AddNumberToObject(root, "total_bytes", (double)state.total_bytes);
    cJSON_AddStringToObject(root, "last_error", esp_err_to_name(state.last_error));
    cJSON_AddBoolToObject(root, "reboot_required", state.reboot_required);
    return send_json_object(req, root);
}

/* ── OTA file upload handlers ────────────────────────────────────────── */

static void ota_restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t ota_upload_firmware_handler(httpd_req_t *req)
{
    ota_state_lock();
    const bool busy = s_ota_state.phase == OTA_UPDATE_RUNNING;
    if (!busy) {
        s_ota_state = (ota_update_state_t){
            .phase = OTA_UPDATE_RUNNING,
            .progress = 0,
            .item_count = 1,
            .last_error = ESP_OK,
        };
        copy_string(s_ota_state.current_label, sizeof(s_ota_state.current_label), "firmware");
        ota_state_set_message("uploading firmware");
    }
    ota_state_unlock();

    if (busy) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA already running");
        return ESP_FAIL;
    }

    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (partition == NULL) {
        ota_state_lock();
        s_ota_state.phase = OTA_UPDATE_FAILED;
        ota_state_set_message("no OTA partition found");
        ota_state_unlock();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ota_state_lock();
        s_ota_state.phase = OTA_UPDATE_FAILED;
        s_ota_state.last_error = err;
        ota_state_set_message("esp_ota_begin failed");
        ota_state_unlock();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_begin failed");
        return ESP_FAIL;
    }

    uint8_t *buf = malloc(OTA_UPLOAD_BUF_SIZE);
    if (buf == NULL) {
        esp_ota_abort(ota_handle);
        ota_state_lock();
        s_ota_state.phase = OTA_UPDATE_FAILED;
        s_ota_state.last_error = ESP_ERR_NO_MEM;
        ota_state_set_message("no memory");
        ota_state_unlock();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }

    size_t total_received = 0;
    size_t remaining = req->content_len;
    bool failed = false;

    while (remaining > 0) {
        const size_t to_read = remaining < OTA_UPLOAD_BUF_SIZE ? remaining : OTA_UPLOAD_BUF_SIZE;
        const int ret = httpd_req_recv(req, (char *)buf, to_read);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            failed = true;
            break;
        }
        err = esp_ota_write(ota_handle, buf, (size_t)ret);
        if (err != ESP_OK) {
            failed = true;
            break;
        }
        total_received += (size_t)ret;
        remaining -= (size_t)ret;

        ota_state_lock();
        s_ota_state.bytes_written = total_received;
        s_ota_state.total_bytes = req->content_len;
        if (req->content_len > 0) {
            s_ota_state.progress = (int)((total_received * 100u) / req->content_len);
        }
        ota_state_unlock();
    }

    free(buf);

    if (failed) {
        esp_ota_abort(ota_handle);
        ota_state_lock();
        s_ota_state.phase = OTA_UPDATE_FAILED;
        s_ota_state.last_error = err != ESP_OK ? err : ESP_FAIL;
        ota_state_set_message(err != ESP_OK ? esp_err_to_name(err) : "upload interrupted");
        ota_state_unlock();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "firmware write failed");
        return ESP_FAIL;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ota_state_lock();
        s_ota_state.phase = OTA_UPDATE_FAILED;
        s_ota_state.last_error = err;
        ota_state_set_message("image validation failed");
        ota_state_unlock();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "firmware validation failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(partition);
    if (err != ESP_OK) {
        ota_state_lock();
        s_ota_state.phase = OTA_UPDATE_FAILED;
        s_ota_state.last_error = err;
        ota_state_set_message("set boot partition failed");
        ota_state_unlock();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set boot partition failed");
        return ESP_FAIL;
    }

    ota_state_lock();
    s_ota_state.phase = OTA_UPDATE_DONE;
    s_ota_state.progress = 100;
    s_ota_state.reboot_required = true;
    ota_state_set_message("firmware uploaded; restarting");
    ota_state_unlock();

    ESP_LOGI(TAG, "Firmware uploaded: %u bytes, restarting...", (unsigned)total_received);
    httpd_resp_send(req, "{\"ok\":true,\"message\":\"firmware uploaded, restarting\"}", -1);

    xTaskCreate(ota_restart_task, "ota_restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t ota_upload_filesystem_handler(httpd_req_t *req)
{
    ota_state_lock();
    const bool busy = s_ota_state.phase == OTA_UPDATE_RUNNING;
    if (!busy) {
        s_ota_state = (ota_update_state_t){
            .phase = OTA_UPDATE_RUNNING,
            .progress = 0,
            .item_count = 1,
            .last_error = ESP_OK,
        };
        copy_string(s_ota_state.current_label, sizeof(s_ota_state.current_label), "filesystem");
        ota_state_set_message("uploading filesystem");
    }
    ota_state_unlock();

    if (busy) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA already running");
        return ESP_FAIL;
    }

    esp_err_t err = esp_vfs_littlefs_unregister("storage");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "littlefs unregister: %s (continuing)", esp_err_to_name(err));
    }

    const esp_partition_t *partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
    if (partition == NULL) {
        ota_state_lock();
        s_ota_state.phase = OTA_UPDATE_FAILED;
        ota_state_set_message("storage partition not found");
        ota_state_unlock();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "storage partition not found");
        goto remount;
    }

    ESP_LOGI(TAG, "Erasing storage partition: %u bytes", (unsigned)partition->size);
    err = esp_partition_erase_range(partition, 0, partition->size);
    if (err != ESP_OK) {
        ota_state_lock();
        s_ota_state.phase = OTA_UPDATE_FAILED;
        s_ota_state.last_error = err;
        ota_state_set_message("erase failed");
        ota_state_unlock();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "partition erase failed");
        goto remount;
    }

    uint8_t *buf = malloc(OTA_UPLOAD_BUF_SIZE);
    if (buf == NULL) {
        ota_state_lock();
        s_ota_state.phase = OTA_UPDATE_FAILED;
        s_ota_state.last_error = ESP_ERR_NO_MEM;
        ota_state_set_message("no memory");
        ota_state_unlock();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        goto remount;
    }

    size_t total_received = 0;
    size_t remaining = req->content_len;
    uint32_t write_offset = 0;
    bool failed = false;

    while (remaining > 0) {
        const size_t to_read = remaining < OTA_UPLOAD_BUF_SIZE ? remaining : OTA_UPLOAD_BUF_SIZE;
        const int ret = httpd_req_recv(req, (char *)buf, to_read);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            failed = true;
            break;
        }
        err = esp_partition_write(partition, write_offset, buf, (size_t)ret);
        if (err != ESP_OK) {
            failed = true;
            break;
        }
        total_received += (size_t)ret;
        write_offset += (uint32_t)ret;
        remaining -= (size_t)ret;

        ota_state_lock();
        s_ota_state.bytes_written = total_received;
        s_ota_state.total_bytes = req->content_len;
        if (req->content_len > 0) {
            s_ota_state.progress = (int)((total_received * 100u) / req->content_len);
        }
        ota_state_unlock();
    }

    free(buf);

    if (failed) {
        ota_state_lock();
        s_ota_state.phase = OTA_UPDATE_FAILED;
        s_ota_state.last_error = err != ESP_OK ? err : ESP_FAIL;
        ota_state_set_message(err != ESP_OK ? esp_err_to_name(err) : "upload interrupted");
        ota_state_unlock();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "filesystem write failed");
        goto remount;
    }

    ota_state_lock();
    s_ota_state.phase = OTA_UPDATE_DONE;
    s_ota_state.progress = 100;
    s_ota_state.reboot_required = true;
    ota_state_set_message("filesystem uploaded; restarting");
    ota_state_unlock();

    ESP_LOGI(TAG, "Filesystem uploaded: %u bytes, restarting...", (unsigned)total_received);
    httpd_resp_send(req, "{\"ok\":true,\"message\":\"filesystem uploaded, restarting\"}", -1);

    xTaskCreate(ota_restart_task, "ota_restart", 2048, NULL, 5, NULL);
    return ESP_OK;

remount:
    {
        const esp_vfs_littlefs_conf_t conf = {
            .base_path = LITTLEFS_BASE_PATH,
            .partition_label = "storage",
            .format_if_mount_failed = true,
            .dont_mount = false,
        };
        esp_err_t remount_err = esp_vfs_littlefs_register(&conf);
        if (remount_err != ESP_OK) {
            ESP_LOGE(TAG, "LittleFS remount failed: %s", esp_err_to_name(remount_err));
        }
    }
    return ESP_FAIL;
}

static esp_err_t debug_json_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json allocation failed");
        return ESP_FAIL;
    }

    /* heap info */
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "min_free_heap", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(root, "largest_free_block",
                            heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    cJSON_AddNumberToObject(root, "internal_free_heap",
                            heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(root, "internal_min_free_heap",
                            heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(root, "internal_largest_free_block",
                            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(root, "psram_free_heap",
                            heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    /* task list via vTaskList (requires configUSE_TRACE_FACILITY) */
#if configUSE_TRACE_FACILITY && configUSE_STATS_FORMATTING_FUNCTIONS
    char *task_buf = malloc(2048);
    if (task_buf != NULL) {
        int hdr = snprintf(task_buf, 2048,
                           "名称            状态  优先级  栈剩余  序号\r\n"
                           "------------------------------------------------\r\n");
        if (hdr > 0 && hdr < 2048) {
            vTaskList(task_buf + hdr);
        }
        cJSON_AddStringToObject(root, "task_list", task_buf);
        free(task_buf);
    }
#else
    cJSON_AddStringToObject(root, "task_list",
        "(需要在 sdkconfig 中启用 CONFIG_FREERTOS_USE_TRACE_FACILITY "
        "和 CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS)");
#endif

    /* uptime */
    cJSON_AddNumberToObject(root, "uptime_ms", esp_timer_get_time() / 1000);

    return send_json_object(req, root);
}

static esp_err_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 16;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;
    config.stack_size = 16384;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_handler,
        };
        httpd_uri_t network_uri = {
            .uri = "/network.json",
            .method = HTTP_GET,
            .handler = network_json_handler,
        };
        httpd_uri_t wifi_config_get_uri = {
            .uri = "/wifi_config.json",
            .method = HTTP_GET,
            .handler = wifi_config_get_handler,
        };
        httpd_uri_t wifi_config_post_uri = {
            .uri = "/wifi_config.json",
            .method = HTTP_POST,
            .handler = wifi_config_post_handler,
        };
        httpd_uri_t debug_uri = {
            .uri = "/debug.json",
            .method = HTTP_GET,
            .handler = debug_json_handler,
        };
        httpd_uri_t ota_start_uri = {
            .uri = "/ota/start",
            .method = HTTP_POST,
            .handler = ota_start_handler,
        };
        httpd_uri_t ota_status_uri = {
            .uri = "/ota/status",
            .method = HTTP_GET,
            .handler = ota_status_handler,
        };
        httpd_uri_t ota_upload_firmware_uri = {
            .uri = "/ota/upload/firmware",
            .method = HTTP_POST,
            .handler = ota_upload_firmware_handler,
        };
        httpd_uri_t ota_upload_filesystem_uri = {
            .uri = "/ota/upload/filesystem",
            .method = HTTP_POST,
            .handler = ota_upload_filesystem_handler,
        };
        esp_err_t reg_err;
        if ((reg_err = httpd_register_uri_handler(server, &root_uri)) != ESP_OK ||
            (reg_err = httpd_register_uri_handler(server, &network_uri)) != ESP_OK ||
            (reg_err = httpd_register_uri_handler(server, &wifi_config_get_uri)) != ESP_OK ||
            (reg_err = httpd_register_uri_handler(server, &wifi_config_post_uri)) != ESP_OK ||
            (reg_err = httpd_register_uri_handler(server, &debug_uri)) != ESP_OK ||
            (reg_err = httpd_register_uri_handler(server, &ota_start_uri)) != ESP_OK ||
            (reg_err = httpd_register_uri_handler(server, &ota_status_uri)) != ESP_OK ||
            (reg_err = httpd_register_uri_handler(server, &ota_upload_firmware_uri)) != ESP_OK ||
            (reg_err = httpd_register_uri_handler(server, &ota_upload_filesystem_uri)) != ESP_OK) {
            ESP_LOGE(TAG, "URI handler registration failed: %s", esp_err_to_name(reg_err));
        }
        if (file_manager_register(server) != ESP_OK) {
            ESP_LOGE(TAG, "file manager registration failed");
        }

        /* Register static file fallback LAST so specific URIs take priority */
        static const httpd_uri_t static_uri = {
            .uri = "/*",
            .method = HTTP_GET,
            .handler = littlefs_static_handler,
        };
        esp_err_t static_err = httpd_register_uri_handler(server, &static_uri);
        if (static_err == ESP_OK) {
            ESP_LOGI(TAG, "Static file fallback registered (/*)");
        } else {
            ESP_LOGW(TAG, "Static fallback registration: %s", esp_err_to_name(static_err));
        }

        s_http_server = server;
        ESP_LOGI(TAG, "HTTP server started on port 80");
    }

    return server == NULL ? ESP_FAIL : ESP_OK;
}

/* Register the static file fallback handler AFTER all other handlers.
 * Call this if you need to add custom handlers after web_server_init_and_start(). */
esp_err_t web_server_register_static_fallback(void)
{
    if (s_http_server == NULL) return ESP_ERR_INVALID_STATE;
    static const httpd_uri_t static_uri = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = littlefs_static_handler,
    };
    esp_err_t err = httpd_register_uri_handler(s_http_server, &static_uri);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Static file fallback registered (/*)");
    }
    return err;
}

/* ── public entry point ──────────────────────────────────────────────── */
esp_err_t web_server_init_and_start(void)
{
    if (s_ota_mutex == NULL) {
        s_ota_mutex = xSemaphoreCreateMutex();
        if (s_ota_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    ESP_ERROR_CHECK(littlefs_init());
    wifi_init();
    return start_webserver();
}
