#include "sd_logger.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <time.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "SD_LOG";

#define ROTATE_INTERVAL_MS 60000u

/* Log directory supplied by the application (SD_LOGGER_DEFAULT_LOG_DIR). */
static char s_log_dir[128];

static FILE             *s_file;
static int               s_seq;
static uint32_t          s_file_start_ms;
static bool              s_time_synced;
static bool              s_had_time_on_open;
static bool              s_in_handler;   /* recursion guard */
static SemaphoreHandle_t s_mutex;
static vprintf_like_t    s_orig_vprintf;
static char              s_current_path[128];

/* ── find next sequence number ──────────────────────────────────────── */
static int find_next_seq(void)
{
    int max_seq = -1;
    DIR *dir = opendir(s_log_dir);
    if (dir == NULL) {
        return 0;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        int seq = 0;
        int parsed_chars = 0;
        if (sscanf(entry->d_name, "log_%d%n", &seq, &parsed_chars) == 1) {
            const char next_ch = entry->d_name[parsed_chars];
            if ((next_ch == '_' || next_ch == '.' || next_ch == '\0') &&
                seq > max_seq) {
                max_seq = seq;
            }
        }
    }
    closedir(dir);
    return max_seq + 1;
}

/* ── generate file path ────────────────────────────────────────────── */
static void generate_path(char *buf, size_t buf_size, int seq)
{
    if (s_time_synced) {
        time_t now = time(NULL);
        if (now > 1700000000) {
            struct tm tm_info;
            localtime_r(&now, &tm_info);
            snprintf(buf, buf_size,
                     "%s/log_%08d_%04d%02d%02d_%02d%02d%02d_UTC8.log",
                     s_log_dir, seq,
                     tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
                     tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
            return;
        }
    }
    snprintf(buf, buf_size, "%s/log_%08d.log", s_log_dir, seq);
}

/* ── close current file and open new one ──────────────────────────── */
static void rotate_file(void)
{
    if (s_file != NULL) {
        fflush(s_file);
        fsync(fileno(s_file));
        fclose(s_file);
        s_file = NULL;
    }

    char path[128];
    int fd;
    while (true) {
        generate_path(path, sizeof(path), s_seq);
        s_seq++;

        fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd >= 0) {
            break;
        }
        if (errno != EEXIST) {
            ESP_LOGE(TAG, "failed to open %s for writing", path);
            s_current_path[0] = '\0';
            s_file_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
            return;
        }
        ESP_LOGW(TAG, "log path already exists, skipping sequence: %s", path);
    }

    s_file = fdopen(fd, "a");
    if (s_file == NULL) {
        close(fd);
        ESP_LOGE(TAG, "failed to create stream for %s", path);
        s_current_path[0] = '\0';
        s_file_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
        return;
    }

    snprintf(s_current_path, sizeof(s_current_path), "%s", path);

    setvbuf(s_file, NULL, _IOLBF, 512);

    s_had_time_on_open = s_time_synced;
    s_file_start_ms = (uint32_t)(esp_timer_get_time() / 1000);

    fprintf(s_file, "\n--- Log file opened ---\n");
    fflush(s_file);
    fsync(fileno(s_file));
}

/* ── custom vprintf: write to UART + SD card ──────────────────────── */
static int sd_log_vprintf(const char *fmt, va_list args)
{
    /* Always write to UART first */
    int uart_len = s_orig_vprintf(fmt, args);

    /* Recursion guard: SD operations may trigger ESP_LOG internally */
    if (s_in_handler || s_file == NULL || s_mutex == NULL) {
        return uart_len;
    }

    s_in_handler = true;

    if (xSemaphoreTake(s_mutex, 0) == pdTRUE) {
        /* Format once into a stack buffer */
        char buf[256];
        va_list args_copy;
        va_copy(args_copy, args);
        int len = vsnprintf(buf, sizeof(buf), fmt, args_copy);
        va_end(args_copy);

        if (len > 0) {
            fwrite(buf, 1, (size_t)len, s_file);
        }

        /* Check rotation */
        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        const bool time_based = (now_ms - s_file_start_ms) >= ROTATE_INTERVAL_MS;
        const bool ntp_just_synced = s_time_synced && !s_had_time_on_open;
        if (time_based || ntp_just_synced) {
            rotate_file();
        }

        xSemaphoreGive(s_mutex);
    }

    s_in_handler = false;
    return uart_len;
}

/* ── public API ─────────────────────────────────────────────────────── */
esp_err_t sd_logger_init_with_config(const sd_logger_config_t *config)
{
    const char *log_dir = (config != NULL && config->log_dir != NULL &&
                           config->log_dir[0] != '\0')
                              ? config->log_dir : SD_LOGGER_DEFAULT_LOG_DIR;
    const int written = snprintf(s_log_dir, sizeof(s_log_dir), "%s", log_dir);
    if (written < 0 || (size_t)written >= sizeof(s_log_dir) ||
        s_log_dir[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }

    struct stat st;
    /* Create log directory; fails cleanly when the SD mount point is absent */
    if (stat(s_log_dir, &st) != 0) {
        if (mkdir(s_log_dir, 0755) != 0) {
            return ESP_FAIL;
        }
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_seq = find_next_seq();
    rotate_file();
    if (s_file == NULL) {
        return ESP_FAIL;
    }

    /* Redirect all ESP log output */
    s_orig_vprintf = esp_log_set_vprintf(sd_log_vprintf);

    return ESP_OK;
}

esp_err_t sd_logger_init(void)
{
    return sd_logger_init_with_config(NULL);
}

void sd_logger_notify_time_synced(void)
{
    if (!s_time_synced) {
        s_time_synced = true;
    }
}

const char *sd_logger_get_current_path(void)
{
    return s_current_path;
}
