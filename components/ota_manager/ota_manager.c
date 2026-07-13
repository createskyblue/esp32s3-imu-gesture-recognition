#include "ota_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_ota_service.h"
#include "esp_partition.h"
#include "esp_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "source/esp_ota_service_source_http.h"
#include "target/esp_ota_service_target_app.h"
#include "target/esp_ota_service_target_data.h"

static const char *TAG = "OTA_MGR";

#define OTA_URL_MAX_BYTES    256u
#define OTA_TASK_STACK_BYTES 8192u
#define OTA_TASK_PRIORITY    4u
#define OTA_UPLOAD_BUF_SIZE  4096u
#define OTA_PARTITION_LABEL_MAX_BYTES 16u

/* ── internal state ────────────────────────────────────────────────────── */
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
    int  progress;
    esp_err_t last_error;
    bool reboot_required;
} ota_state_t;

static SemaphoreHandle_t s_mutex;
static ota_state_t s_state = { .phase = OTA_UPDATE_IDLE, .message = "idle" };
static struct {
    char filesystem_partition_label[OTA_PARTITION_LABEL_MAX_BYTES + 1u];
    ota_filesystem_update_callback_t filesystem_update_begin;
    ota_filesystem_update_callback_t filesystem_update_end;
    void *context;
} s_config;

/* For synchronous upload operations */
static esp_ota_handle_t       s_upload_ota_handle;
static const esp_partition_t *s_upload_partition;
static uint32_t               s_upload_write_offset;
static bool                   s_upload_fs_active;

/* ── helpers ───────────────────────────────────────────────────────────── */
static void copy_str(char *dest, size_t dest_size, const char *src)
{
    if (dest == NULL || dest_size == 0u) return;
    if (src == NULL) { dest[0] = '\0'; return; }
    snprintf(dest, dest_size, "%s", src);
}

static void lock(void)   { if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY); }
static void unlock(void) { if (s_mutex) xSemaphoreGive(s_mutex); }

static void set_message(const char *msg) { copy_str(s_state.message, sizeof(s_state.message), msg); }

static bool filesystem_update_configured(void)
{
    return s_config.filesystem_partition_label[0] != '\0' &&
           s_config.filesystem_update_begin != NULL &&
           s_config.filesystem_update_end != NULL;
}

static esp_err_t begin_filesystem_update(void)
{
    if (!filesystem_update_configured()) return ESP_ERR_NOT_SUPPORTED;
    return s_config.filesystem_update_begin(s_config.context);
}

static esp_err_t end_filesystem_update(void)
{
    if (!filesystem_update_configured()) return ESP_ERR_NOT_SUPPORTED;
    return s_config.filesystem_update_end(s_config.context);
}

/* ── OTA service event handler ─────────────────────────────────────────── */
static void ota_event_handler(const adf_event_t *event, void *ctx)
{
    (void)ctx;
    if (event == NULL || event->payload == NULL || event->payload_len < sizeof(esp_ota_service_event_t))
        return;

    const esp_ota_service_event_t *e = (const esp_ota_service_event_t *)event->payload;
    lock();
    switch (e->id) {
    case ESP_OTA_SERVICE_EVT_SESSION_BEGIN:
        s_state.phase = OTA_UPDATE_RUNNING;
        s_state.progress = 1;
        set_message("OTA session started");
        break;
    case ESP_OTA_SERVICE_EVT_ITEM_BEGIN:
        s_state.item_index = e->item_index + 1u;
        copy_str(s_state.current_label, sizeof(s_state.current_label),
                 e->item_label ? e->item_label : "item");
        s_state.bytes_written = 0u;
        s_state.total_bytes = 0u;
        set_message("downloading");
        break;
    case ESP_OTA_SERVICE_EVT_ITEM_PROGRESS:
        s_state.bytes_written = e->progress.bytes_written;
        s_state.total_bytes = e->progress.total_bytes;
        if (e->progress.total_bytes > 0u) {
            uint32_t pct = (uint32_t)((e->progress.bytes_written * 100u) / e->progress.total_bytes);
            s_state.progress = (int)(((s_state.item_index - 1u) * 100u + pct) / s_state.item_count);
        }
        set_message("writing flash");
        break;
    case ESP_OTA_SERVICE_EVT_ITEM_END:
        s_state.last_error = e->error;
        if (e->error != ESP_OK) {
            s_state.phase = OTA_UPDATE_FAILED;
            s_state.progress = 100;
            set_message(esp_err_to_name(e->error));
        }
        break;
    case ESP_OTA_SERVICE_EVT_SESSION_END:
        if (e->session_end.aborted || e->session_end.failed_count > 0u) {
            s_state.phase = OTA_UPDATE_FAILED;
            set_message("OTA failed");
        } else {
            s_state.phase = OTA_UPDATE_DONE;
            s_state.progress = 100;
            s_state.reboot_required = (s_state.firmware_url[0] != '\0' || s_state.filesystem_url[0] != '\0');
            set_message(s_state.reboot_required ? "done; reboot to apply" : "done");
        }
        break;
    default:
        break;
    }
    unlock();
}

/* ── OTA item preparation ──────────────────────────────────────────────── */
static esp_err_t prepare_item(esp_ota_upgrade_item_t *item, const char *label,
                               const char *uri, bool filesystem)
{
    (void)label;
    if (item == NULL || uri == NULL || uri[0] == '\0') return ESP_ERR_INVALID_ARG;

    esp_ota_service_source_t *source = NULL;
    esp_ota_service_target_t *target = NULL;
    esp_err_t err = esp_ota_service_source_http_create(NULL, &source);
    if (err != ESP_OK) return err;

    if (filesystem) {
        if (!filesystem_update_configured()) {
            if (source->destroy) source->destroy(source);
            return ESP_ERR_NOT_SUPPORTED;
        }
        err = esp_ota_service_target_data_create(NULL, &target);
        item->partition_label = s_config.filesystem_partition_label;
        item->resumable = false;
    } else {
        esp_ota_service_target_app_cfg_t app_cfg = { .bulk_flash_erase = true };
        err = esp_ota_service_target_app_create(&app_cfg, &target);
        item->resumable = true;
    }
    if (err != ESP_OK) {
        if (source && source->destroy) source->destroy(source);
        return err;
    }
    item->uri = uri;
    item->source = source;
    item->target = target;
    item->skip_on_fail = false;
    return ESP_OK;
}

/* ── background OTA task (URL-based) ───────────────────────────────────── */
static void ota_task(void *arg)
{
    (void)arg;
    char fw_url[OTA_URL_MAX_BYTES + 1u];
    char fs_url[OTA_URL_MAX_BYTES + 1u];

    lock();
    copy_str(fw_url, sizeof(fw_url), s_state.firmware_url);
    copy_str(fs_url, sizeof(fs_url), s_state.filesystem_url);
    s_state.phase = OTA_UPDATE_RUNNING;
    s_state.progress = 1;
    s_state.last_error = ESP_OK;
    set_message("starting OTA");
    unlock();

    esp_err_t err = ESP_OK;
    bool filesystem_update_active = false;
    if (fs_url[0] != '\0') {
        err = begin_filesystem_update();
        if (err != ESP_OK) goto done;
        filesystem_update_active = true;
    }

    esp_ota_service_cfg_t cfg = ESP_OTA_SERVICE_CFG_DEFAULT();
    cfg.worker_task.stack_size = OTA_TASK_STACK_BYTES;
    cfg.worker_task.priority = OTA_TASK_PRIORITY;
    esp_ota_service_t *service = NULL;
    err = esp_ota_service_create(&cfg, &service);
    if (err != ESP_OK) goto done;

    esp_ota_upgrade_item_t items[2] = {0};
    int item_count = 0;
    if (fw_url[0] != '\0') {
        err = prepare_item(&items[item_count++], "firmware", fw_url, false);
        if (err != ESP_OK) goto destroy;
    }
    if (fs_url[0] != '\0') {
        err = prepare_item(&items[item_count++], "filesystem", fs_url, true);
        if (err != ESP_OK) goto destroy;
    }

    lock(); s_state.item_count = (uint32_t)item_count; unlock();

    adf_event_subscribe_info_t sub = ADF_EVENT_SUBSCRIBE_INFO_DEFAULT();
    sub.handler = ota_event_handler;
    err = esp_service_event_subscribe((esp_service_t *)service, &sub);
    if (err != ESP_OK) goto destroy;

    err = esp_ota_service_set_upgrade_list(service, items, item_count);
    if (err == ESP_OK) err = esp_service_start((esp_service_t *)service);
    if (err == ESP_OK) {
        while (1) {
            lock();
            bool finished = (s_state.phase == OTA_UPDATE_DONE || s_state.phase == OTA_UPDATE_FAILED);
            unlock();
            if (finished) break;
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

destroy:
    if (service != NULL) esp_ota_service_destroy(service);
done:
    if (filesystem_update_active) {
        const esp_err_t storage_err = end_filesystem_update();
        if (err == ESP_OK && storage_err != ESP_OK) err = storage_err;
    }
    if (err != ESP_OK) {
        lock();
        s_state.phase = OTA_UPDATE_FAILED;
        s_state.progress = 100;
        s_state.last_error = err;
        s_state.reboot_required = false;
        set_message(esp_err_to_name(err));
        unlock();
    }
    vTaskDelete(NULL);
}

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

/* ══════════════════════════════════════════════════════════════════════════
 * Public API
 * ══════════════════════════════════════════════════════════════════════════ */

esp_err_t ota_manager_init(void)
{
    return ota_manager_init_with_config(NULL);
}

esp_err_t ota_manager_init_with_config(const ota_manager_config_t *config)
{
    if (s_mutex != NULL) return ESP_ERR_INVALID_STATE;

    memset(&s_config, 0, sizeof(s_config));
    if (config != NULL) {
        if (config->filesystem_partition_label == NULL ||
            config->filesystem_update_begin == NULL ||
            config->filesystem_update_end == NULL) {
            return ESP_ERR_INVALID_ARG;
        }
        const size_t label_length = strnlen(
            config->filesystem_partition_label,
            sizeof(s_config.filesystem_partition_label));
        if (label_length == 0u ||
            label_length > OTA_PARTITION_LABEL_MAX_BYTES) {
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(s_config.filesystem_partition_label,
               config->filesystem_partition_label, label_length + 1u);
        s_config.filesystem_update_begin = config->filesystem_update_begin;
        s_config.filesystem_update_end = config->filesystem_update_end;
        s_config.context = config->context;
    }

    s_mutex = xSemaphoreCreateMutex();
    return s_mutex ? ESP_OK : ESP_ERR_NO_MEM;
}

void ota_manager_get_status(ota_status_t *out)
{
    if (out == NULL) return;
    lock();
    out->phase         = (ota_phase_t)s_state.phase;
    copy_str(out->current_label, sizeof(out->current_label), s_state.current_label);
    copy_str(out->message, sizeof(out->message), s_state.message);
    out->progress      = s_state.progress;
    out->item_index    = s_state.item_index;
    out->item_count    = s_state.item_count;
    out->bytes_written = s_state.bytes_written;
    out->total_bytes   = s_state.total_bytes;
    out->last_error    = s_state.last_error;
    out->reboot_required = s_state.reboot_required;
    unlock();
}

bool ota_manager_is_busy(void)
{
    lock();
    bool busy = (s_state.phase == OTA_UPDATE_RUNNING);
    unlock();
    return busy;
}

esp_err_t ota_manager_start_url(const char *firmware_url, const char *filesystem_url)
{
    if (filesystem_url != NULL && filesystem_url[0] != '\0' &&
        !filesystem_update_configured()) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    lock();
    if (s_state.phase == OTA_UPDATE_RUNNING) { unlock(); return ESP_ERR_INVALID_STATE; }
    s_state = (ota_state_t){
        .phase = OTA_UPDATE_RUNNING,
        .progress = 0,
        .item_count = (firmware_url && firmware_url[0] ? 1u : 0u) +
                      (filesystem_url && filesystem_url[0] ? 1u : 0u),
        .last_error = ESP_OK,
    };
    copy_str(s_state.firmware_url, sizeof(s_state.firmware_url),
             firmware_url ? firmware_url : "");
    copy_str(s_state.filesystem_url, sizeof(s_state.filesystem_url),
             filesystem_url ? filesystem_url : "");
    set_message("queued");
    unlock();

    if (xTaskCreate(ota_task, "ota_update", OTA_TASK_STACK_BYTES, NULL,
                    OTA_TASK_PRIORITY, NULL) != pdPASS) {
        lock();
        s_state.phase = OTA_UPDATE_FAILED;
        s_state.last_error = ESP_ERR_NO_MEM;
        set_message("failed to create OTA task");
        unlock();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ── firmware upload ───────────────────────────────────────────────────── */
esp_err_t ota_manager_upload_firmware_begin(void)
{
    lock();
    if (s_state.phase == OTA_UPDATE_RUNNING) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_state = (ota_state_t){
        .phase = OTA_UPDATE_RUNNING, .progress = 0,
        .item_count = 1, .last_error = ESP_OK,
    };
    copy_str(s_state.current_label, sizeof(s_state.current_label), "firmware");
    set_message("uploading firmware");
    unlock();

    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (partition == NULL) {
        lock();
        s_state.phase = OTA_UPDATE_FAILED;
        set_message("no OTA partition found");
        unlock();
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = esp_ota_begin(partition, OTA_WITH_SEQUENTIAL_WRITES, &s_upload_ota_handle);
    if (err != ESP_OK) {
        lock();
        s_state.phase = OTA_UPDATE_FAILED;
        s_state.last_error = err;
        set_message("esp_ota_begin failed");
        unlock();
        return err;
    }
    return ESP_OK;
}

esp_err_t ota_manager_upload_firmware_write(const uint8_t *data, size_t len)
{
    esp_err_t err = esp_ota_write(s_upload_ota_handle, data, len);
    if (err == ESP_OK) {
        lock();
        s_state.bytes_written += len;
        unlock();
    }
    return err;
}

esp_err_t ota_manager_upload_firmware_end(void)
{
    esp_err_t err = esp_ota_end(s_upload_ota_handle);
    if (err != ESP_OK) {
        lock();
        s_state.phase = OTA_UPDATE_FAILED;
        s_state.last_error = err;
        set_message("image validation failed");
        unlock();
        return err;
    }

    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    err = esp_ota_set_boot_partition(partition);
    if (err != ESP_OK) {
        lock();
        s_state.phase = OTA_UPDATE_FAILED;
        s_state.last_error = err;
        set_message("set boot partition failed");
        unlock();
        return err;
    }

    lock();
    s_state.phase = OTA_UPDATE_DONE;
    s_state.progress = 100;
    s_state.reboot_required = true;
    set_message("firmware uploaded; restarting");
    unlock();
    return ESP_OK;
}

void ota_manager_upload_firmware_abort(void)
{
    esp_ota_abort(s_upload_ota_handle);
    lock();
    s_state.phase = OTA_UPDATE_FAILED;
    s_state.last_error = ESP_FAIL;
    set_message("firmware upload aborted");
    unlock();
}

/* ── filesystem upload ─────────────────────────────────────────────────── */
esp_err_t ota_manager_upload_fs_begin(void)
{
    lock();
    if (s_state.phase == OTA_UPDATE_RUNNING) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_state = (ota_state_t){
        .phase = OTA_UPDATE_RUNNING, .progress = 0,
        .item_count = 1, .last_error = ESP_OK,
    };
    copy_str(s_state.current_label, sizeof(s_state.current_label), "filesystem");
    set_message("uploading filesystem");
    unlock();

    esp_err_t err = begin_filesystem_update();
    if (err != ESP_OK) {
        lock();
        s_state.phase = OTA_UPDATE_FAILED;
        s_state.last_error = err;
        set_message("filesystem lease failed");
        unlock();
        return err;
    }
    s_upload_fs_active = true;

    s_upload_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_ANY, s_config.filesystem_partition_label);
    if (s_upload_partition == NULL) {
        err = ESP_ERR_NOT_FOUND;
        lock();
        s_state.phase = OTA_UPDATE_FAILED;
        s_state.last_error = err;
        set_message("storage partition not found");
        unlock();
        goto release_storage;
    }

    ESP_LOGI(TAG, "Erasing storage partition: %u bytes", (unsigned)s_upload_partition->size);
    err = esp_partition_erase_range(s_upload_partition, 0, s_upload_partition->size);
    if (err != ESP_OK) {
        lock();
        s_state.phase = OTA_UPDATE_FAILED;
        s_state.last_error = err;
        set_message("erase failed");
        unlock();
        goto release_storage;
    }

    s_upload_write_offset = 0;
    return ESP_OK;

release_storage:
    {
        const esp_err_t storage_err = end_filesystem_update();
        s_upload_fs_active = false;
        s_upload_partition = NULL;
        if (err == ESP_OK) err = storage_err;
    }
    return err;
}

esp_err_t ota_manager_upload_fs_write(const uint8_t *data, size_t len)
{
    if (!s_upload_fs_active || s_upload_partition == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (data == NULL || len == 0u) return ESP_ERR_INVALID_ARG;
    if (s_upload_write_offset > s_upload_partition->size ||
        len > s_upload_partition->size - s_upload_write_offset) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = esp_partition_write(s_upload_partition, s_upload_write_offset, data, len);
    if (err == ESP_OK) {
        s_upload_write_offset += (uint32_t)len;
        lock();
        s_state.bytes_written += len;
        unlock();
    }
    return err;
}

esp_err_t ota_manager_upload_fs_end(void)
{
    if (!s_upload_fs_active) return ESP_ERR_INVALID_STATE;

    const esp_err_t storage_err = end_filesystem_update();
    s_upload_fs_active = false;
    s_upload_partition = NULL;

    lock();
    if (storage_err != ESP_OK) {
        s_state.phase = OTA_UPDATE_FAILED;
        s_state.last_error = storage_err;
        set_message("filesystem remount failed");
        unlock();
        return storage_err;
    }
    s_state.phase = OTA_UPDATE_DONE;
    s_state.progress = 100;
    s_state.reboot_required = true;
    set_message("filesystem uploaded; restarting");
    unlock();
    return ESP_OK;
}

void ota_manager_upload_fs_abort(void)
{
    esp_err_t storage_err = ESP_OK;
    if (s_upload_fs_active) {
        storage_err = end_filesystem_update();
        s_upload_fs_active = false;
        s_upload_partition = NULL;
    }

    lock();
    s_state.phase = OTA_UPDATE_FAILED;
    s_state.last_error = storage_err == ESP_OK ? ESP_FAIL : storage_err;
    set_message(storage_err == ESP_OK ? "filesystem upload aborted"
                                      : "filesystem remount failed");
    unlock();
}

/* ── restart ───────────────────────────────────────────────────────────── */
void ota_manager_restart(void)
{
    xTaskCreate(restart_task, "ota_restart", 2048, NULL, 5, NULL);
}

/* ══════════════════════════════════════════════════════════════════════════
 * HTTP handlers (registered by ota_manager_register)
 * ══════════════════════════════════════════════════════════════════════════ */

#define OTA_JSON_BUFFER_BYTES  768u
#define OTA_UPLOAD_BUF_SIZE    4096u

static esp_err_t receive_body(httpd_req_t *req, char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0u || req->content_len >= buffer_size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json body too large");
        return ESP_FAIL;
    }
    size_t received = 0u;
    while (received < req->content_len) {
        const int ret = httpd_req_recv(req, buffer + received, req->content_len - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed to receive body");
            return ESP_FAIL;
        }
        received += (size_t)ret;
    }
    buffer[received] = '\0';
    return ESP_OK;
}

static esp_err_t json_response(httpd_req_t *req, cJSON *root)
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
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    esp_err_t err = httpd_resp_sendstr(req, json);
    cJSON_free(json);
    return err;
}

/* ── POST /ota/start ─────────────────────────────────────────────────── */
static esp_err_t ota_start_handler(httpd_req_t *req)
{
    char body[OTA_JSON_BUFFER_BYTES];
    if (receive_body(req, body, sizeof(body)) != ESP_OK) return ESP_FAIL;

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    char fw_url[257] = {0};
    char fs_url[257] = {0};
    cJSON *fw = cJSON_GetObjectItemCaseSensitive(root, "firmware_url");
    cJSON *fs = cJSON_GetObjectItemCaseSensitive(root, "filesystem_url");
    if (cJSON_IsString(fw) && fw->valuestring) snprintf(fw_url, sizeof(fw_url), "%s", fw->valuestring);
    if (cJSON_IsString(fs) && fs->valuestring) snprintf(fs_url, sizeof(fs_url), "%s", fs->valuestring);
    cJSON_Delete(root);

    if (fw_url[0] == '\0' && fs_url[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected firmware_url or filesystem_url");
        return ESP_FAIL;
    }

    esp_err_t err = ota_manager_start_url(fw_url, fs_url);
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA already running");
        return ESP_FAIL;
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed to start OTA");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"OTA started\"}");
}

/* ── GET /ota/status ─────────────────────────────────────────────────── */
static esp_err_t ota_status_handler(httpd_req_t *req)
{
    ota_status_t st;
    ota_manager_get_status(&st);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "phase", st.phase == OTA_PHASE_IDLE    ? "idle" :
                                           st.phase == OTA_PHASE_RUNNING ? "running" :
                                           st.phase == OTA_PHASE_DONE    ? "done" : "failed");
    cJSON_AddStringToObject(root, "message", st.message);
    cJSON_AddStringToObject(root, "current_label", st.current_label);
    cJSON_AddNumberToObject(root, "progress", st.progress);
    cJSON_AddNumberToObject(root, "item_index", st.item_index);
    cJSON_AddNumberToObject(root, "item_count", st.item_count);
    cJSON_AddNumberToObject(root, "bytes_written", (double)st.bytes_written);
    cJSON_AddNumberToObject(root, "total_bytes", (double)st.total_bytes);
    cJSON_AddStringToObject(root, "last_error", esp_err_to_name(st.last_error));
    cJSON_AddBoolToObject(root, "reboot_required", st.reboot_required);
    return json_response(req, root);
}

/* ── POST /ota/upload/firmware ───────────────────────────────────────── */
static esp_err_t ota_upload_firmware_handler(httpd_req_t *req)
{
    if (ota_manager_is_busy()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA already running");
        return ESP_FAIL;
    }

    esp_err_t err = ota_manager_upload_firmware_begin();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    uint8_t *buf = malloc(OTA_UPLOAD_BUF_SIZE);
    if (buf == NULL) {
        ota_manager_upload_firmware_abort();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }

    size_t total = 0;
    size_t remaining = req->content_len;
    bool failed = false;
    while (remaining > 0) {
        const size_t n = remaining < OTA_UPLOAD_BUF_SIZE ? remaining : OTA_UPLOAD_BUF_SIZE;
        const int ret = httpd_req_recv(req, (char *)buf, n);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
            failed = true; break;
        }
        err = ota_manager_upload_firmware_write(buf, (size_t)ret);
        if (err != ESP_OK) { failed = true; break; }
        total += (size_t)ret;
        remaining -= (size_t)ret;
    }
    free(buf);

    if (failed) {
        ota_manager_upload_firmware_abort();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "firmware write failed");
        return ESP_FAIL;
    }

    err = ota_manager_upload_firmware_end();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "firmware validation failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Firmware uploaded: %u bytes, restarting...", (unsigned)total);
    httpd_resp_send(req, "{\"ok\":true,\"message\":\"firmware uploaded, restarting\"}", -1);
    ota_manager_restart();
    return ESP_OK;
}

/* ── POST /ota/upload/filesystem ─────────────────────────────────────── */
static esp_err_t ota_upload_filesystem_handler(httpd_req_t *req)
{
    if (ota_manager_is_busy()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA already running");
        return ESP_FAIL;
    }

    esp_err_t err = ota_manager_upload_fs_begin();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "filesystem OTA begin failed");
        return ESP_FAIL;
    }

    uint8_t *buf = malloc(OTA_UPLOAD_BUF_SIZE);
    if (buf == NULL) {
        ota_manager_upload_fs_abort();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }

    size_t total = 0;
    size_t remaining = req->content_len;
    bool failed = false;
    while (remaining > 0) {
        const size_t n = remaining < OTA_UPLOAD_BUF_SIZE ? remaining : OTA_UPLOAD_BUF_SIZE;
        const int ret = httpd_req_recv(req, (char *)buf, n);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
            failed = true; break;
        }
        err = ota_manager_upload_fs_write(buf, (size_t)ret);
        if (err != ESP_OK) { failed = true; break; }
        total += (size_t)ret;
        remaining -= (size_t)ret;
    }
    free(buf);

    if (failed) {
        ota_manager_upload_fs_abort();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "filesystem write failed");
        return ESP_FAIL;
    }

    err = ota_manager_upload_fs_end();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "filesystem OTA end failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Filesystem uploaded: %u bytes, restarting...", (unsigned)total);
    httpd_resp_send(req, "{\"ok\":true,\"message\":\"filesystem uploaded, restarting\"}", -1);
    ota_manager_restart();
    return ESP_OK;
}

/* ── registration ────────────────────────────────────────────────────── */
esp_err_t ota_manager_register(httpd_handle_t server)
{
    const httpd_uri_t start_uri  = { .uri = "/ota/start",            .method = HTTP_POST, .handler = ota_start_handler };
    const httpd_uri_t status_uri = { .uri = "/ota/status",           .method = HTTP_GET,  .handler = ota_status_handler };
    const httpd_uri_t fw_uri     = { .uri = "/ota/upload/firmware",   .method = HTTP_POST, .handler = ota_upload_firmware_handler };
    const httpd_uri_t fs_uri     = { .uri = "/ota/upload/filesystem", .method = HTTP_POST, .handler = ota_upload_filesystem_handler };

    esp_err_t err;
    if ((err = httpd_register_uri_handler(server, &start_uri))  != ESP_OK) return err;
    if ((err = httpd_register_uri_handler(server, &status_uri)) != ESP_OK) return err;
    if ((err = httpd_register_uri_handler(server, &fw_uri))     != ESP_OK) return err;
    if ((err = httpd_register_uri_handler(server, &fs_uri))     != ESP_OK) return err;

    ESP_LOGI(TAG, "OTA handlers registered");
    return ESP_OK;
}
