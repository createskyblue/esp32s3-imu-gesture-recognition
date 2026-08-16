#include "gesture_demo.h"

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "lvgl.h"

#include "lcd_lvgl.h"
#include "qmi8658.h"
#include "gesture_classifier.h"
#include <dirent.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"

/* ═════════════════════════════ 常量 ═════════════════════════════ */

#define TAG "GESTURE"

#define SAMPLE_COUNT       100   /* 2 s @ 50 Hz */
#define SAMPLE_PERIOD_MS   20    /* 50 Hz 采样周期 */
#define BOOT_BTN_GPIO      GPIO_NUM_0

/* ±4g 量程：raw(LSB) → 毫g(mG, 整数, 供 LVGL chart 使用) */
#define RAW_TO_MG          (1000.0f / 8192.0f)

#define CSV_DIR            "/sdcard"
#define CSV_PREFIX_A       "gesture_A_"
#define CSV_PREFIX_B       "gesture_B_"
#define CSV_PREFIX_C       "gesture_C_"
#define CSV_SUFFIX         ".csv"

/* 截图 */
#define SHOT_DIR          "/sdcard/screenshots"

/* UI 布局 (320x240) */
#define UI_CHART_W         202
#define UI_CHART_H         176
#define UI_CHART_X         4
#define UI_CHART_Y         4
#define UI_PANEL_X         210
#define UI_PANEL_W         106
#define UI_BTN_Y           184
#define UI_BTN_H           52
#define UI_BTN_W           80

/* 推理输入：100 样本 × 3 轴交错（与 CSV 行格式一致，单位 g） */
#define INFER_AXES         3

typedef enum {
    GESTURE_A = 0,
    GESTURE_B,
    GESTURE_C,
    GESTURE_COUNT,
} gesture_id_t;

/* ═════════════════════════════ 状态（跨任务共享） ═════════════════════════════
 * s_samples / s_capture_done 由采集任务写、UI 任务读，用互斥锁保护；
 * s_selected / s_file_lines 为简单整型，32 位对齐读写天然原子，可容忍轻微竞态。
 */

static qmi8658_accel_raw_t s_samples[SAMPLE_COUNT];
static volatile bool s_capture_done = false;   /* 新一次采集完成待 UI 取走 */
static volatile bool s_capture_busy = false;   /* 正在采样中 */
static SemaphoreHandle_t s_samples_mutex = NULL;

static volatile int s_selected = GESTURE_A;    /* 当前选中的手势按钮 */
/* 推理模式：Infer 按钮切换；开启后 BOOT 采样的结果交给分类器（不写 CSV） */
static volatile bool s_infer_mode = false;     /* 推理模式（A/B/C 按钮退出） */
static volatile bool s_infer_done = false;     /* 推理完成待 UI 取走 */
static volatile int  s_infer_result = -1;      /* 类 id：0=C,1=B,2=A；-1=失败 */
static int s_file_lines[GESTURE_COUNT] = {0, 0, 0};
static int32_t s_boot_count = 1;
static char s_csv_path[GESTURE_COUNT][64];

static QueueHandle_t s_btn_queue = NULL;

/* LVGL 对象：只能在 display/LVGL 任务里访问 */
static lv_obj_t *s_chart = NULL;
static lv_chart_series_t *s_series[3];
static int32_t s_disp[3][SAMPLE_COUNT];        /* mG，图表外置数据源 */
static lv_obj_t *s_mm_label[3];                /* 每轴 max/min 文本 */
static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_btn[3];
static lv_obj_t *s_btn_label[3];
static lv_obj_t *s_btn_infer = NULL;
static lv_obj_t *s_infer_overlay = NULL;       /* 图表区域叠加的半透明结果字母 */
/* 截图：LVGL 任务取快照入队，写卡任务编码 BMP 存 SD（写卡不阻塞 UI） */
typedef struct {
    void       *buf;      /* RGB565 快照数据（PSRAM） */
    uint32_t    w, h;     /* 图像宽高（像素） */
    uint32_t    stride;   /* 每行字节数 */
} shot_job_t;

static QueueHandle_t s_shot_queue = NULL;
static volatile bool s_shot_busy = false;   /* 上一张还没写完，忽略新点击 */
static volatile bool s_shot_done = false;   /* 写卡任务已完成，待 UI 刷新提示 */
static SemaphoreHandle_t s_shot_lock = NULL;
static char s_shot_msg[48];

static const char *s_axis_name[3] = { "X", "Y", "Z" };
static const lv_color_t s_axis_color[3] = {
    LV_COLOR_MAKE(0xE0, 0x30, 0x30),  /* X 红 */
    LV_COLOR_MAKE(0x20, 0xC0, 0x30),  /* Y 绿 */
    LV_COLOR_MAKE(0x30, 0x70, 0xF0),  /* Z 蓝 */
};

/* ═════════════════════════════ CSV ═════════════════════════════ */

/* 每次开机新建 A/B/C 三个 CSV 文件（文件名带开机序号，天然互不覆盖） */
static void prepare_csv_files(void)
{
    static const char *prefix[GESTURE_COUNT] = { CSV_PREFIX_A, CSV_PREFIX_B, CSV_PREFIX_C };

    for (int i = 0; i < GESTURE_COUNT; i++) {
        snprintf(s_csv_path[i], sizeof(s_csv_path[i]), "%s/%s%d%s",
                 CSV_DIR, prefix[i], (int)s_boot_count, CSV_SUFFIX);
        FILE *f = fopen(s_csv_path[i], "w");   /* 覆盖旧文件 = 新建 */
        if (f == NULL) {
            ESP_LOGW(TAG, "create %s failed (SD not ready?)", s_csv_path[i]);
            continue;
        }
        fclose(f);
        ESP_LOGI(TAG, "created fresh CSV: %s", s_csv_path[i]);
    }
}

/* 统计一个 CSV 已有多少行（样本行），用于按钮标签 */
static int count_csv_lines(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return 0;
    }
    int lines = 0;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') {
            lines++;
        }
    }
    fclose(f);
    return lines;
}

/* 追加一行：x1,y1,z1,x2,y2,z2,...,x100,y100,z100（交错存储，参考 Nano Edge AI） */
static void append_csv_row(int sel, const qmi8658_accel_raw_t *samples, int n)
{
    FILE *f = fopen(s_csv_path[sel], "a");
    if (f == NULL) {
        ESP_LOGE(TAG, "append %s failed", s_csv_path[sel]);
        return;
    }
    for (int i = 0; i < n; i++) {
        fprintf(f, "%s%.6f,%.6f,%.6f",
                i ? "," : "",
                qmi8658_raw_to_g(samples[i].acc_x),
                qmi8658_raw_to_g(samples[i].acc_y),
                qmi8658_raw_to_g(samples[i].acc_z));
    }
    fprintf(f, "\n");
    fclose(f);
    ESP_LOGI(TAG, "row appended -> %s (line %d)", s_csv_path[sel], s_file_lines[sel]);
}

/* ═════════════════════════════ NVS 开机计数 ═════════════════════════════ */

static esp_err_t nvs_boot_count(void)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open("gesture_demo", NVS_READWRITE, &h), TAG, "nvs_open");

    esp_err_t err = nvs_get_i32(h, "boot_cnt", &s_boot_count);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_boot_count = 1;
        err = ESP_OK;
    }
    if (err != ESP_OK) {
        nvs_close(h);
        return err;
    }
    s_boot_count++;
    err = nvs_set_i32(h, "boot_cnt", s_boot_count);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "boot counter = %d", (int)s_boot_count);
    }
    return err;
}

/* ═════════════════════════════ BOOT 按键 ═════════════════════════════ */

static void IRAM_ATTR boot_btn_isr(void *arg)
{
    uint32_t gpio = (uint32_t)(uintptr_t)arg;
    BaseType_t wake = pdFALSE;
    xQueueSendFromISR(s_btn_queue, &gpio, &wake);
    if (wake == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void boot_btn_init(void)
{
    gpio_config_t io = {
        .intr_type = GPIO_INTR_NEGEDGE,   /* 按下 = 低电平下降沿 */
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = 1ULL << BOOT_BTN_GPIO,
        .pull_down_en = 0,
        .pull_up_en = 1,                  /* 外部上拉 + 内部上拉，防抖 */
    };
    gpio_config(&io);

    s_btn_queue = xQueueCreate(10, sizeof(uint32_t));
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "gpio_install_isr_service: %s", esp_err_to_name(err));
    }
    gpio_isr_handler_add(BOOT_BTN_GPIO, boot_btn_isr, (void *)(uintptr_t)BOOT_BTN_GPIO);
}

/* ═════════════════════════════ 采集任务 ═════════════════════════════
 * 只做：等 BOOT 按键 → 50Hz 采 2s → 按模式（写 CSV / 推理）→ 发布数据给 UI。
 * 不碰任何 LVGL 对象。推理是模式（Infer 按钮切换），物理 BOOT 按键始终是采样触发源。
 */

static void capture_task(void *arg)
{
    (void)arg;
    qmi8658_accel_raw_t tmp[SAMPLE_COUNT];

    for (;;) {
        uint32_t gpio = 0;
        if (xQueueReceive(s_btn_queue, &gpio, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (s_capture_busy) {
            ESP_LOGW(TAG, "capture in progress, ignore BOOT press");
            continue;
        }
        /* 简单消抖：等 30ms 后确认按键仍为低 */
        vTaskDelay(pdMS_TO_TICKS(30));
        if (gpio_get_level(BOOT_BTN_GPIO) != 0) {
            continue;
        }

        s_capture_busy = true;
        ESP_LOGI(TAG, "capturing 2s @ 50Hz (100 samples)...");
        const bool do_infer = s_infer_mode;    /* 采集开始时定格模式 */

        /* vTaskDelayUntil 保证精确 20ms 采样间隔（50Hz） */
        TickType_t last = xTaskGetTickCount();
        for (int i = 0; i < SAMPLE_COUNT; i++) {
            if (qmi8658_read_accel_raw(&tmp[i]) != ESP_OK) {
                ESP_LOGW(TAG, "sample %d read failed", i);
            }
            vTaskDelayUntil(&last, pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
        }

        if (do_infer) {
            /* 推理：交错 float in[300]（单位 g），完成后不写 CSV */
            float in[SAMPLE_COUNT * INFER_AXES];
            float probs[INFER_AXES];
            int id = -1;
            for (int i = 0; i < SAMPLE_COUNT; i++) {
                in[i * 3 + 0] = qmi8658_raw_to_g(tmp[i].acc_x);
                in[i * 3 + 1] = qmi8658_raw_to_g(tmp[i].acc_y);
                in[i * 3 + 2] = qmi8658_raw_to_g(tmp[i].acc_z);
            }
            if (gc_classification(in, probs, &id) == GC_OK && id >= 0 && id < INFER_AXES) {
                ESP_LOGI(TAG, "infer -> %s (prob %.3f)",
                         gc_get_class_name(id), (double)probs[id]);
            } else {
                id = -1;
                ESP_LOGE(TAG, "gc_classification failed");
            }

            /* 发布给 UI（互斥锁内一次性拷贝 + 置完成标志；同时发布波形供图表显示） */
            if (s_samples_mutex != NULL &&
                xSemaphoreTake(s_samples_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                s_infer_result = id;
                s_infer_done = true;
                memcpy(s_samples, tmp, sizeof(s_samples));
                s_capture_done = true;
                xSemaphoreGive(s_samples_mutex);
            }
            ESP_LOGI(TAG, "infer: stack_highwater=%u heap_free=%u",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL),
                     (unsigned)esp_get_free_heap_size());
            ESP_LOGI(TAG, "inference done");
        } else {
            const int sel = s_selected;
            append_csv_row(sel, tmp, SAMPLE_COUNT);
            s_file_lines[sel]++;

            /* 发布给 UI（互斥锁内一次性拷贝 + 置完成标志） */
            if (s_samples_mutex != NULL &&
                xSemaphoreTake(s_samples_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                memcpy(s_samples, tmp, sizeof(s_samples));
                s_capture_done = true;
                xSemaphoreGive(s_samples_mutex);
            }
            ESP_LOGI(TAG, "capture done");
        }
        s_capture_busy = false;
    }
}

/* ═════════════════════════════ 屏幕截图 → BMP 存 SD ═════════════════════════════
 * 点击图表 → LVGL 任务里 lv_snapshot 全屏快照（RGB565, PSRAM）→ 队列
 * 交给 shot_writer 任务编码为 24 位 BMP 保存到 /sdcard/screenshots/。
 */

static void set_shot_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (s_shot_lock != NULL && xSemaphoreTake(s_shot_lock, portMAX_DELAY) == pdTRUE) {
        vsnprintf(s_shot_msg, sizeof(s_shot_msg), fmt, ap);
        xSemaphoreGive(s_shot_lock);
    }
    va_end(ap);
}

/* 扫描截图目录，返回下一个可用序号（已有最大 shot_N.bmp + 1） */
static int next_shot_number(void)
{
    int max = 0;
    DIR *d = opendir(SHOT_DIR);
    if (d != NULL) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            int n = 0;
            if (sscanf(e->d_name, "shot_%d.bmp", &n) == 1 && n > max) {
                max = n;
            }
        }
        closedir(d);
    }
    return max + 1;
}

/* RGB565 → 24 位 BMP（自底向上行序；320*3=960 字节/行，无需对齐填充） */
static esp_err_t save_bmp(const void *rgb565, uint32_t w, uint32_t h, uint32_t stride)
{
    mkdir(SHOT_DIR, 0777);   /* 已存在则忽略失败 */

    char path[64];
    snprintf(path, sizeof(path), "%s/shot_%03d.bmp", SHOT_DIR, next_shot_number());

    const uint32_t row_bytes = w * 3;
    const uint32_t pixel_bytes = row_bytes * h;
    const uint32_t file_size = 54 + pixel_bytes;

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "open %s failed (SD not ready?)", path);
        return ESP_FAIL;
    }

    uint8_t hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = file_size & 0xFF;        hdr[3] = (file_size >> 8) & 0xFF;
    hdr[4] = (file_size >> 16) & 0xFF; hdr[5] = (file_size >> 24) & 0xFF;
    hdr[10] = 54;                     /* 像素数据偏移 */
    hdr[14] = 40;                     /* BITMAPINFOHEADER 大小 */
    hdr[18] = w & 0xFF;               hdr[19] = (w >> 8) & 0xFF;
    hdr[20] = (w >> 16) & 0xFF;       hdr[21] = (w >> 24) & 0xFF;
    hdr[22] = h & 0xFF;               hdr[23] = (h >> 8) & 0xFF;
    hdr[24] = (h >> 16) & 0xFF;       hdr[25] = (h >> 24) & 0xFF;
    hdr[26] = 1;                      /* planes */
    hdr[28] = 24;                     /* bpp */
    hdr[34] = pixel_bytes & 0xFF;     hdr[35] = (pixel_bytes >> 8) & 0xFF;
    hdr[36] = (pixel_bytes >> 16) & 0xFF; hdr[37] = (pixel_bytes >> 24) & 0xFF;

    if (fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f);
        return ESP_FAIL;
    }

    uint8_t *rowbuf = malloc(row_bytes);
    if (rowbuf == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    const uint8_t *base = rgb565;
    for (uint32_t y = 0; y < h; y++) {
        const uint16_t *row = (const uint16_t *)(base + (h - 1 - y) * stride);
        for (uint32_t x = 0; x < w; x++) {
            uint16_t px = row[x];
            uint8_t r5 = (px >> 11) & 0x1F;
            uint8_t g6 = (px >> 5) & 0x3F;
            uint8_t b5 = px & 0x1F;
            rowbuf[x * 3 + 0] = (uint8_t)((b5 << 3) | (b5 >> 2)); /* B */
            rowbuf[x * 3 + 1] = (uint8_t)((g6 << 2) | (g6 >> 4)); /* G */
            rowbuf[x * 3 + 2] = (uint8_t)((r5 << 3) | (r5 >> 2)); /* R */
        }
        if (fwrite(rowbuf, 1, row_bytes, f) != row_bytes) {
            free(rowbuf);
            fclose(f);
            return ESP_FAIL;
        }
    }
    free(rowbuf);
    fclose(f);
    const uint16_t *px0 = (const uint16_t *)rgb565;   /* 屏幕左上角像素（诊断用） */
    ESP_LOGI(TAG, "screenshot saved: %s (%u B, corner 0x%04X)",
             path, (unsigned)file_size, (unsigned)px0[0]);
    return ESP_OK;
}

static void shot_writer_task(void *arg)
{
    (void)arg;
    shot_job_t job;

    for (;;) {
        if (xQueueReceive(s_shot_queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        esp_err_t err = save_bmp(job.buf, job.w, job.h, job.stride);
        set_shot_msg(err == ESP_OK ? "Shot saved" : "Shot failed");
        s_shot_done = true;
        heap_caps_free(job.buf);
        s_shot_busy = false;
    }
}

/* 点击图表 → 全屏快照 → 交给写卡任务（快照必须在 LVGL 任务里做） */
static void chart_event_cb(lv_event_t *e)
{
    (void)e;
    if (s_shot_busy || s_shot_queue == NULL) {
        return;
    }

    lv_obj_t *scr = lv_scr_act();
    const uint32_t w = (uint32_t)lv_obj_get_width(scr);
    const uint32_t h = (uint32_t)lv_obj_get_height(scr);
    const uint32_t stride = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_RGB565);
    const uint32_t size = stride * h;

    void *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGE(TAG, "no PSRAM for screenshot");
        lv_label_set_text(s_status_label, "Shot: no mem");
        return;
    }

    lv_draw_buf_t db;
    if (lv_draw_buf_init(&db, w, h, LV_COLOR_FORMAT_RGB565, stride, buf, size) != LV_RESULT_OK ||
        lv_snapshot_take_to_draw_buf(scr, LV_COLOR_FORMAT_RGB565, &db) != LV_RESULT_OK) {
        ESP_LOGE(TAG, "snapshot failed");
        heap_caps_free(buf);
        lv_label_set_text(s_status_label, "Shot failed");
        return;
    }

    shot_job_t job = {
        .buf = buf,
        .w = db.header.w,
        .h = db.header.h,
        .stride = db.header.stride,
    };
    if (xQueueSend(s_shot_queue, &job, 0) != pdTRUE) {
        heap_caps_free(buf);
        lv_label_set_text(s_status_label, "Shot busy");
        return;
    }
    s_shot_busy = true;
    lv_label_set_text(s_status_label, "Saving shot...");
}

/* ═════════════════════════════ LVGL UI（display 任务上下文） ═════════════════════════════ */

static void btn_event_cb(lv_event_t *e)
{
    const int id = (int)(intptr_t)lv_event_get_user_data(e);

    if (id < GESTURE_COUNT) {           /* A / B / C：选 CSV 手势类别，退出推理模式 */
        s_infer_mode = false;
        lv_obj_add_flag(s_infer_overlay, LV_OBJ_FLAG_HIDDEN);
        s_selected = id;
        for (int i = 0; i < GESTURE_COUNT; i++) {
            lv_obj_set_style_bg_color(s_btn[i],
                                      (i == id) ? lv_color_hex(0x0060A0) : lv_color_hex(0x30343A),
                                      0);
        }
        lv_obj_set_style_bg_color(s_btn_infer, lv_color_hex(0x503000), 0);   /* 取消推理高亮 */
        lv_label_set_text(s_status_label, "Ready");
    } else {                            /* 推理：切换为推理模式，BOOT 按键触发采样（不写 CSV） */
        s_infer_mode = true;
        s_infer_done = false;
        s_infer_result = -1;
        lv_obj_add_flag(s_infer_overlay, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < GESTURE_COUNT; i++) {
            lv_obj_set_style_bg_color(s_btn[i], lv_color_hex(0x30343A), 0);
        }
        lv_obj_set_style_bg_color(s_btn_infer, lv_color_hex(0x0060A0), 0);   /* 高亮推理 */
        lv_label_set_text(s_status_label, "Infer: press BOOT");
    }
}

static void ui_timer_cb(lv_timer_t *timer)
{
    bool have_new = false;
    bool infer_new = false;
    int  infer_id = -1;

    /* 取走最新一次采集/推理结果（只拷贝，不做 LVGL 调用，锁内停留极短） */
    if (s_samples_mutex != NULL && xSemaphoreTake(s_samples_mutex, 0) == pdTRUE) {
        if (s_capture_done) {
            for (int ax = 0; ax < 3; ax++) {
                for (int i = 0; i < SAMPLE_COUNT; i++) {
                    const int16_t raw = (ax == 0) ? s_samples[i].acc_x
                                        : (ax == 1) ? s_samples[i].acc_y
                                                    : s_samples[i].acc_z;
                    s_disp[ax][i] = (int32_t)((float)raw * RAW_TO_MG);
                }
            }
            s_capture_done = false;
            have_new = true;
        }
        if (s_infer_done) {
            s_infer_done = false;
            infer_id = s_infer_result;
            infer_new = true;
        }
        xSemaphoreGive(s_samples_mutex);
    }

    if (have_new) {
        /* 刷新三轴波形 */
        lv_chart_refresh(s_chart);

        /* 右侧 max/min（单位 g） */
        for (int ax = 0; ax < 3; ax++) {
            int32_t mx = INT32_MIN, mn = INT32_MAX;
            for (int i = 0; i < SAMPLE_COUNT; i++) {
                if (s_disp[ax][i] > mx) mx = s_disp[ax][i];
                if (s_disp[ax][i] < mn) mn = s_disp[ax][i];
            }
            char buf[40];
            snprintf(buf, sizeof(buf), "%s max %+.2f\n   min %+.2f",
                     s_axis_name[ax], (float)mx / 1000.0f, (float)mn / 1000.0f);
            lv_label_set_text(s_mm_label[ax], buf);
        }

        /* 按钮行数标签 + 状态（推理模式不写 CSV，行数不变） */
        if (!infer_new) {
            char lbl[16];
            snprintf(lbl, sizeof(lbl), "%c:%d", 'A' + s_selected, s_file_lines[s_selected]);
            lv_label_set_text(s_btn_label[s_selected], lbl);
            lv_label_set_text(s_status_label, s_infer_mode ? "Inferring..." : "Saved to SD");
        }
    } else if (s_capture_busy) {
        lv_label_set_text(s_status_label, s_infer_mode ? "Inferring..." : "Sampling...");
    } else if (strcmp(lv_label_get_text(s_status_label), "Sampling...") == 0 ||
               strcmp(lv_label_get_text(s_status_label), "Inferring...") == 0) {
        lv_label_set_text(s_status_label, "Ready");
    }

    /* 推理结果：大字透明字母叠加到图表区域（类 id：0=C,1=B,2=A）。
     * 结果只在图表上显示，右侧面板只给中性状态，不显示识别结果。 */
    if (infer_new) {
        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);
        ESP_LOGI(TAG, "lvmem before overlay: free=%u biggest=%u frag=%u%%",
                 (unsigned)mon.free_size, (unsigned)mon.free_biggest_size, (unsigned)mon.frag_pct);
        if (infer_id >= 0 && infer_id < INFER_AXES) {
            static const char *letters[INFER_AXES] = { "C", "B", "A" };
            /* A=红, B=绿, C=蓝（半透明柔和色，id 映射 0=C,1=B,2=A） */
            switch (infer_id) {
            case 0:  lv_obj_set_style_text_color(s_infer_overlay, lv_color_hex(0x40A0FF), 0); break; /* C -> 蓝 */
            case 1:  lv_obj_set_style_text_color(s_infer_overlay, lv_color_hex(0x40FF40), 0); break; /* B -> 绿 */
            default: lv_obj_set_style_text_color(s_infer_overlay, lv_color_hex(0xFF5040), 0); break; /* A -> 红 */
            }
            lv_label_set_text(s_infer_overlay, letters[infer_id]);
            lv_obj_update_layout(s_infer_overlay);      /* 先算好内容尺寸 */
            const int32_t ow = lv_obj_get_width(s_infer_overlay);
            const int32_t oh = lv_obj_get_height(s_infer_overlay);
            const int32_t glyph_bottom = oh - lv_font_montserrat_48.base_line; /* 字形底部=基线 */
            lv_obj_set_style_transform_pivot_x(s_infer_overlay, ow, 0);        /* 锚定字形右下角 */
            lv_obj_set_style_transform_pivot_y(s_infer_overlay, glyph_bottom, 0);
            lv_obj_set_style_transform_scale(s_infer_overlay, 512, 0);         /* 2x 向左上放大 */
            lv_obj_align(s_infer_overlay, LV_ALIGN_BOTTOM_RIGHT, -6, oh - glyph_bottom - 6);
            lv_obj_clear_flag(s_infer_overlay, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_status_label, "Infer done");
        } else {
            lv_label_set_text(s_status_label, "Infer: failed");
        }
        lv_mem_monitor(&mon);
        ESP_LOGI(TAG, "lvmem after overlay: free=%u biggest=%u frag=%u%%",
                 (unsigned)mon.free_size, (unsigned)mon.free_biggest_size, (unsigned)mon.frag_pct);
    }

    /* 截图保存结果提示（写卡任务回填，与采集状态互不覆盖） */
    if (s_shot_done) {
        s_shot_done = false;
        char msg[sizeof(s_shot_msg)];
        if (s_shot_lock != NULL && xSemaphoreTake(s_shot_lock, 0) == pdTRUE) {
            memcpy(msg, s_shot_msg, sizeof(msg));
            xSemaphoreGive(s_shot_lock);
        } else {
            msg[0] = '\0';
        }
        lv_label_set_text(s_status_label, msg);
    }
    (void)timer;
}

void gesture_demo_ui_create(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

    /* ── 左侧：三轴波形图（无渐变填充，纯折线） ── */
    s_chart = lv_chart_create(scr);
    lv_obj_set_size(s_chart, UI_CHART_W, UI_CHART_H);
    lv_obj_set_pos(s_chart, UI_CHART_X, UI_CHART_Y);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, SAMPLE_COUNT);
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, -4000, 4000); /* ±4g */
    lv_chart_set_div_line_count(s_chart, 4, 5);

    /* 折线宽 2px，不画数据点（LVGL9：系列样式统一挂在 chart 的 PART 上） */
    lv_obj_set_style_line_width(s_chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(s_chart, 0, 0, LV_PART_INDICATOR);

    for (int i = 0; i < 3; i++) {
        s_series[i] = lv_chart_add_series(s_chart, s_axis_color[i], LV_CHART_AXIS_PRIMARY_Y);
        lv_chart_set_series_ext_y_array(s_chart, s_series[i], s_disp[i]);
    }

    /* 点击图表 → 屏幕截图保存到 SD 卡 */
    lv_obj_add_event_cb(s_chart, chart_event_cb, LV_EVENT_CLICKED, NULL);

    /* 推理结果叠加层：图表区域内居中、半透明大字母（默认隐藏；label 不可点击，不挡截图） */
    s_infer_overlay = lv_label_create(s_chart);
    lv_label_set_text(s_infer_overlay, "A");
    lv_obj_set_style_text_font(s_infer_overlay, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_infer_overlay, lv_color_hex(0x000000), 0);  /* 黑色水印 */
    lv_obj_set_style_text_opa(s_infer_overlay, LV_OPA_50, 0);                 /* 半透明 */
    lv_obj_align(s_infer_overlay, LV_ALIGN_BOTTOM_RIGHT, -6, -6);
    lv_obj_add_flag(s_infer_overlay, LV_OBJ_FLAG_HIDDEN);

    /* ── 右侧：每轴 max/min + 状态 ── */
    lv_obj_t *panel = lv_obj_create(scr);
    lv_obj_set_size(panel, UI_PANEL_W, UI_CHART_H);
    lv_obj_set_pos(panel, UI_PANEL_X, UI_CHART_Y);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x181C22), 0);
    lv_obj_set_style_pad_all(panel, 6, 0);
    lv_obj_set_style_pad_top(panel, 6, 0);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "Max / Min (g)");
    lv_obj_set_style_text_color(title, lv_color_hex(0xAAAAAA), 0);

    for (int i = 0; i < 3; i++) {
        s_mm_label[i] = lv_label_create(panel);
        lv_label_set_text(s_mm_label[i], "X max --\n   min --");
        lv_obj_set_style_text_color(s_mm_label[i], s_axis_color[i], 0);
        lv_obj_set_style_text_line_space(s_mm_label[i], 0, 0);
    }
    s_status_label = lv_label_create(panel);
    lv_label_set_text(s_status_label, "Ready");
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x80D080), 0);

    /* 右侧面板垂直排布 */
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(panel, 4, 0);

    /* ── 底部：A / B / C / 推理 四个等宽按钮（flex 行布局，按钮间留 4px 间距） ── */
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_set_size(row, 320, UI_BTN_H);
    lv_obj_set_pos(row, 0, UI_BTN_Y);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(row, 2, 0);
    lv_obj_set_style_pad_column(row, 4, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);

    static const char *btn_names[3] = { "A", "B", "C" };
    for (int i = 0; i < 3; i++) {
        s_btn[i] = lv_button_create(row);
        lv_obj_set_size(s_btn[i], 0, UI_BTN_H - 4);
        lv_obj_set_flex_grow(s_btn[i], 1);
        lv_obj_add_event_cb(s_btn[i], btn_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_set_style_bg_color(s_btn[i],
                                  (i == s_selected) ? lv_color_hex(0x0060A0) : lv_color_hex(0x30343A),
                                  0);

        s_btn_label[i] = lv_label_create(s_btn[i]);
        char lbl[16];
        snprintf(lbl, sizeof(lbl), "%s:%d", btn_names[i], s_file_lines[i]);
        lv_label_set_text(s_btn_label[i], lbl);
        lv_obj_center(s_btn_label[i]);
    }

    s_btn_infer = lv_button_create(row);
    lv_obj_set_size(s_btn_infer, 0, UI_BTN_H - 4);
    lv_obj_set_flex_grow(s_btn_infer, 1);
    lv_obj_add_event_cb(s_btn_infer, btn_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)GESTURE_COUNT);
    lv_obj_set_style_bg_color(s_btn_infer, lv_color_hex(0x503000), 0);
    lv_obj_t *infer_lbl = lv_label_create(s_btn_infer);
    lv_label_set_text(infer_lbl, "Infer");
    lv_obj_center(infer_lbl);

    /* 周期轮询采集完成标志（LV_OS_NONE：LVGL 单线程，定时器运行在 display 任务） */
    lv_timer_create(ui_timer_cb, 20, NULL);
}

/* ═════════════════════════════ 启动 ═════════════════════════════ */

esp_err_t gesture_demo_start(void)
{
    /* NVS：可重复调用（已初始化时返回 ESP_OK），失败也不阻塞启动 */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err == ESP_OK) {
        nvs_boot_count();
    } else {
        ESP_LOGW(TAG, "NVS init failed: %s; use boot_count=1", esp_err_to_name(err));
        s_boot_count = 1;
    }

    /* 每次开机新建 A/B/C 三个 CSV 文件 */
    prepare_csv_files();
    for (int i = 0; i < GESTURE_COUNT; i++) {
        s_file_lines[i] = count_csv_lines(s_csv_path[i]);
    }

    /* IMU 挂在 LCD 的同一根 I2C 总线上 */
    i2c_master_bus_handle_t bus = NULL;
    if (lcd_lvgl_get_i2c_bus(&bus) != ESP_OK || bus == NULL) {
        ESP_LOGE(TAG, "I2C bus not ready (call after lcd_lvgl_start)");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t imu_err = qmi8658_init(bus);
    if (imu_err != ESP_OK) {
        ESP_LOGE(TAG, "QMI8658 init failed: %s", esp_err_to_name(imu_err));
        return imu_err;
    }

    s_samples_mutex = xSemaphoreCreateMutex();

    /* 截图：LVGL 任务取快照入队 → 写卡任务编码 BMP 存 SD */
    s_shot_lock = xSemaphoreCreateMutex();
    s_shot_queue = xQueueCreate(2, sizeof(shot_job_t));
    if (xTaskCreate(shot_writer_task, "shot_save", 6144, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "shot writer task create failed");
        return ESP_ERR_NO_MEM;
    }

    /* 推理分类器：纯 C 无动态分配，内部峰值栈约 4~5KB（采集任务栈已加大） */
    if (gc_init() != GC_OK) {
        ESP_LOGE(TAG, "gesture classifier init failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "gesture classifier ready (classes: A/B/C)");

    boot_btn_init();

    if (xTaskCreate(capture_task, "gesture_cap", 16384, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "gesture demo started (BOOT: 2s capture; A/B/C=CSV mode, Infer=classifier mode)");
    return ESP_OK;
}






