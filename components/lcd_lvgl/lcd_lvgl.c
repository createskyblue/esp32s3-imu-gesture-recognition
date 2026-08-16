#include "lcd_lvgl.h"

#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_log.h"
#include "esp_private/esp_clk.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

/* ═══════════════ 立创实战派 ESP32-S3 引脚定义 ═══════════════ */

/* I2C (PCA9557 IO expander / QMI8658 IMU share this bus) */
#define BSP_I2C_SDA          GPIO_NUM_1
#define BSP_I2C_SCL          GPIO_NUM_2
#define BSP_I2C_FREQ_HZ      (100000)

/* PCA9557 IO expander (address 0x19) */
#define PCA9557_SENSOR_ADDR         0x19
#define PCA9557_INPUT_PORT          0x00
#define PCA9557_OUTPUT_PORT         0x01
#define PCA9557_POLARITY_PORT       0x02
#define PCA9557_CONFIGURATION_PORT  0x03
#define PCA9557_LCD_CS_BIT          (1u << 0)
#define PCA9557_PA_EN_BIT           (1u << 1)
#define PCA9557_DVP_PWDN_BIT        (1u << 2)

/* LCD ST7789, 320x240, SPI */
#define LCD_H_RES           320
#define LCD_V_RES           240
#define LCD_SPI_HOST        SPI3_HOST
#define LCD_PIXEL_CLOCK_HZ  (80 * 1000 * 1000)
#define LCD_PIN_MOSI        GPIO_NUM_40
#define LCD_PIN_SCLK        GPIO_NUM_41
#define LCD_PIN_CS          GPIO_NUM_NC   /* CS is driven by PCA9557 */
#define LCD_PIN_DC          GPIO_NUM_39
#define LCD_PIN_RST         GPIO_NUM_NC
#define LCD_PIN_BACKLIGHT   GPIO_NUM_42
#define LCD_BITS_PER_PIXEL  16

/* LEDC backlight: low-speed mode, ch0/timer1, 10-bit, 5 kHz, inverted */
#define LCD_LEDC_MODE       LEDC_LOW_SPEED_MODE
#define LCD_LEDC_CH         LEDC_CHANNEL_0
#define LCD_LEDC_TIMER      LEDC_TIMER_1
#define LCD_LEDC_DUTY_RES   LEDC_TIMER_10_BIT
#define LCD_LEDC_FREQ_HZ    5000
#define LCD_LEDC_MAX_DUTY   ((1u << 10) - 1u)

/* LVGL: two full-frame draw buffers in PSRAM. A full-frame buffer makes the
 * flush a single SPI transaction per frame, so the SPI transfer of frame N
 * overlaps the rendering of frame N+1 -> ~60 fps at 80 MHz SPI (measured
 * 62.9 fps / 15.9 ms per full-screen frame). */
#define LCD_BUFFER_ROWS     240
#define LCD_BUFFER_BYTES    (LCD_H_RES * LCD_BUFFER_ROWS * sizeof(uint16_t))

#define DISPLAY_TASK_STACK_BYTES 16384u
#define DISPLAY_TASK_PRIORITY    10u
/* How often the display task polls lv_timer_handler. Must be well below the
 * LVGL refresh period (16 ms) so a 60 Hz refresh actually gets serviced. */
#define DISPLAY_REFRESH_MS       5u

static const char *TAG = "LCD_LVGL";

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_pca9557_dev = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_panel_io_handle_t s_io = NULL;
static lcd_lvgl_screen_builder_t s_screen_builder = NULL;

/* Posted from the esp_lcd DMA-done callback so lcd_flush_wait_cb() can
 * block (instead of LVGL's default busy-loop) while a frame is draining. */
static SemaphoreHandle_t s_flush_done = NULL;

#if CONFIG_LCD_LVGL_BENCHMARK
/* Raw SPI throughput benchmark (heap buffers, bypasses LVGL). */
static void raw_spi_benchmark(void)
{
    ESP_LOGI(TAG, "DBG: CPU=%d MHz APB=%d MHz", esp_clk_cpu_freq()/1000000,
             esp_clk_apb_freq()/1000000);

    /* Full frame from PSRAM (this is what LVGL buffers use). */
    uint8_t *full = heap_caps_malloc(320 * 240 * 2, MALLOC_CAP_SPIRAM);
    if (!full) { ESP_LOGE(TAG, "raw spi: malloc failed"); return; }
    ESP_LOGI(TAG, "RAW SPI buf=%p align=%u", (void*)full, (unsigned)((uintptr_t)full & 0x3F));
    memset(full, 0x33, 320*240*2);
    const int nf = 30;
    uint64_t t0 = esp_timer_get_time();
    for (int i = 0; i < nf; i++) esp_lcd_panel_draw_bitmap(s_panel, 0, 0, 320, 240, full);
    uint64_t t1 = esp_timer_get_time();
    ESP_LOGI(TAG, "RAW SPI full(PSRAM): %.3f ms/block -> %.2f MB/s", (double)(t1-t0)/1e3/nf,
             (double)(320*240*2) / ((double)(t1-t0)/1e6/nf) / 1e6);

    /* 60-row block from DRAM: isolates pure SPI line rate (no PSRAM read). */
    uint8_t *dram = heap_caps_malloc(320 * 60 * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (dram) {
        memset(dram, 0x55, 320*60*2);
        uint64_t d0 = esp_timer_get_time();
        for (int i = 0; i < nf; i++) esp_lcd_panel_draw_bitmap(s_panel, 0, 0, 320, 60, dram);
        uint64_t d1 = esp_timer_get_time();
        ESP_LOGI(TAG, "RAW SPI 60row(DRAM): %.3f ms/block -> %.2f MB/s", (double)(d1-d0)/1e3/nf,
                 (double)(320*60*2) / ((double)(d1-d0)/1e6/nf) / 1e6);
        heap_caps_free(dram);
    } else {
        ESP_LOGW(TAG, "raw spi: DRAM malloc failed (skip)");
    }

    heap_caps_free(full);
}
#endif /* CONFIG_LCD_LVGL_BENCHMARK */


#if CONFIG_LCD_LVGL_BENCHMARK
/* Full-screen refresh benchmark counters (updated from flush callback) */
static uint64_t s_flush_bytes = 0;
static uint64_t s_flush_us = 0;
static uint32_t s_flush_count = 0;
#endif /* CONFIG_LCD_LVGL_BENCHMARK */

/* ═══════════════ I2C (new driver API) ═══════════════ */

static esp_err_t bsp_i2c_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus),
                        TAG, "I2C bus init failed");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCA9557_SENSOR_ADDR,
        .scl_speed_hz = BSP_I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg,
                                                  &s_pca9557_dev),
                        TAG, "PCA9557 device add failed");
    ESP_LOGI(TAG, "I2C ready (SDA=1, SCL=2, 100 kHz)");
    return ESP_OK;
}

/* ═══════════════ PCA9557 IO expander ═══════════════ */

static esp_err_t pca9557_write_byte(uint8_t reg, uint8_t data)
{
    uint8_t buf[2] = { reg, data };
    return i2c_master_transmit(s_pca9557_dev, buf, sizeof(buf), 100);
}

static esp_err_t pca9557_read_byte(uint8_t reg, uint8_t *data)
{
    return i2c_master_transmit_receive(s_pca9557_dev, &reg, 1, data, 1, 100);
}

static esp_err_t pca9557_init(void)
{
    /* Default outputs: DVP_PWDN=1, PA_EN=0, LCD_CS=1 */
    ESP_RETURN_ON_ERROR(pca9557_write_byte(PCA9557_OUTPUT_PORT, 0x05),
                        TAG, "PCA9557 output write failed");
    /* IO0-2 configured as outputs, the rest stay inputs */
    ESP_RETURN_ON_ERROR(pca9557_write_byte(PCA9557_CONFIGURATION_PORT, 0xf8),
                        TAG, "PCA9557 config write failed");
    return ESP_OK;
}

static esp_err_t lcd_cs_set(bool level)
{
    uint8_t data = 0;
    ESP_RETURN_ON_ERROR(pca9557_read_byte(PCA9557_OUTPUT_PORT, &data),
                        TAG, "PCA9557 read failed");
    if (level) {
        data |= PCA9557_LCD_CS_BIT;
    } else {
        data &= (uint8_t)~PCA9557_LCD_CS_BIT;
    }
    return pca9557_write_byte(PCA9557_OUTPUT_PORT, data);
}

/* ═══════════════ LEDC backlight ═══════════════ */

static esp_err_t bsp_display_backlight_init(void)
{
    const ledc_timer_config_t timer_cfg = {
        .speed_mode = LCD_LEDC_MODE,
        .duty_resolution = LCD_LEDC_DUTY_RES,
        .timer_num = LCD_LEDC_TIMER,
        .freq_hz = LCD_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg),
                        TAG, "LEDC timer config failed");

    const ledc_channel_config_t ch_cfg = {
        .gpio_num = LCD_PIN_BACKLIGHT,
        .speed_mode = LCD_LEDC_MODE,
        .channel = LCD_LEDC_CH,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LCD_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = true,   /* backlight is active-low */
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch_cfg),
                        TAG, "LEDC channel config failed");
    return ESP_OK;
}

static esp_err_t bsp_display_backlight_set(int brightness_percent)
{
    if (brightness_percent < 0) brightness_percent = 0;
    if (brightness_percent > 100) brightness_percent = 100;
    const uint32_t duty =
        (LCD_LEDC_MAX_DUTY * (uint32_t)brightness_percent) / 100u;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LCD_LEDC_MODE, LCD_LEDC_CH, duty),
                        TAG, "LEDC set duty failed");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LCD_LEDC_MODE, LCD_LEDC_CH),
                        TAG, "LEDC update duty failed");
    return ESP_OK;
}

/* ═══════════════ ST7789 panel ═══════════════ */

static esp_err_t lcd_panel_init(void)
{
    const spi_bus_config_t bus_cfg = {
        .sclk_io_num = LCD_PIN_SCLK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &bus_cfg,
                                           SPI_DMA_CH_AUTO),
                        TAG, "SPI bus init failed");

    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = LCD_PIN_CS,       /* GPIO_NUM_NC: CS via PCA9557 */
        .dc_gpio_num = LCD_PIN_DC,
        .spi_mode = 2,                   /* board wiring uses SPI mode 2 */
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 4,   /* deeper queue: pipeline SPI with LVGL rendering */
        /* Let DMA read the LVGL draw buffers straight from PSRAM. The default
         * path memcpy()s every flush into internal RAM first, which measured
         * ~7.4 MB/s; direct PSRAM DMA reaches ~10 MB/s (80 MHz line limit)
         * and is the key to sustaining 60 fps. */
        .flags.psram_dma_direct = true,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(LCD_SPI_HOST, &io_cfg, &io),
                        TAG, "New SPI panel IO failed");
    s_io = io;   /* keep handle: register the DMA-done callback after LVGL init */

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BITS_PER_PIXEL,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel),
                        TAG, "New ST7789 panel failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "Panel reset failed");
    ESP_RETURN_ON_ERROR(lcd_cs_set(false), TAG, "LCD CS low failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "Panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true),
                        TAG, "Invert color failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, true),
                        TAG, "Swap xy failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, true, false),
                        TAG, "Mirror failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true),
                        TAG, "Display on failed");
    ESP_RETURN_ON_ERROR(bsp_display_backlight_set(100),
                        TAG, "Backlight on failed");
    ESP_LOGI(TAG, "ST7789 panel ready (%ux%u @ %u Hz)",
             LCD_H_RES, LCD_V_RES, (unsigned)LCD_PIXEL_CLOCK_HZ);
    return ESP_OK;
}

/* ═══════════════ LVGL 9 ═══════════════ */

static uint32_t lv_tick_get_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

/* Called from the esp_lcd SPI task once GDMA has really finished sending the
 * frame. Only then is it safe for LVGL to reuse the draw buffer — the flush
 * callback itself only queues an async SPI transaction. */
static bool lcd_flush_ready_cb(esp_lcd_panel_io_handle_t io,
                               esp_lcd_panel_io_event_data_t *edata,
                               void *user_ctx)
{
    (void)io;
    (void)edata;
    lv_display_t *disp = (lv_display_t *)user_ctx;
    lv_display_flush_ready(disp);
    if (s_flush_done != NULL) {
        xSemaphoreGive(s_flush_done);
    }
    return true;
}

/* LVGL calls this instead of its default `while (disp->flushing);` busy
 * loop. We block on the DMA-done semaphore, so the display task sleeps
 * (0% CPU) instead of spinning while a frame is being pushed to the panel. */
static void lcd_flush_wait_cb(lv_display_t *disp)
{
    (void)disp;
    if (s_flush_done != NULL) {
        xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(100));
    }
}

static void lcd_flush_cb(lv_display_t *disp, const lv_area_t *area,
                         uint8_t *px_map)
{
    (void)disp;
    /* LVGL renders directly in RGB565_SWAPPED (big-endian byte order) to
     * match the ST7789, so the buffer is handed to esp_lcd as-is, zero-copy. */
#if CONFIG_LCD_LVGL_BENCHMARK
    const size_t count = (size_t)(area->x2 - area->x1 + 1) *
                         (size_t)(area->y2 - area->y1 + 1);
    const uint64_t flush_t0 = esp_timer_get_time();
#endif
    /* Queue the frame: DMA reads it from PSRAM while LVGL renders the next
     * one; lv_display_flush_ready() fires from lcd_flush_ready_cb(). */
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, px_map);
#if CONFIG_LCD_LVGL_BENCHMARK
    s_flush_bytes += (uint64_t)count * sizeof(uint16_t);
    s_flush_us += esp_timer_get_time() - flush_t0;
    s_flush_count++;
#endif
}

/* Full-screen HSV demo: the whole panel cycles through the hue wheel together
 * (with a gentle brightness breathing). Only the screen background color
 * changes, so LVGL renders it with a fast fill (no per-pixel canvas work) ?
 * that keeps CPU low and lets the real ~60 fps show in LVGL's built-in
 * performance monitor (top-left "FPS/CPU" window). */
static lv_obj_t *s_demo_screen = NULL;

static void demo_hsv_timer_cb(lv_timer_t *timer)
{
    static uint32_t tick = 0;

    /* Whole panel shares one hue; full 360deg cycle every ~12 s at 60 fps. */
    const uint16_t hue = (uint16_t)((tick / 2u) % 360u);
    /* Brightness breathes ~199..255 so colors stay vivid, never grey. */
    const uint8_t value = (uint8_t)(227 + (28 * lv_trigo_sin((int16_t)((tick * 3) % 3600)) / 1000));

    lv_obj_set_style_bg_color(s_demo_screen, lv_color_hsv_to_rgb(hue, 255, value), 0);
    lv_obj_invalidate(s_demo_screen);

    tick++;
    (void)timer;
}

static void demo_screen_create(void)
{
    /* Application UI takes over when a screen builder is registered. */
    if (s_screen_builder != NULL) {
        s_screen_builder();
        return;
    }
    s_demo_screen = lv_scr_act();
    lv_obj_set_style_bg_color(s_demo_screen, lv_color_hex(0x000000), 0);
    lv_timer_t *t = lv_timer_create(demo_hsv_timer_cb, 16, NULL);
    lv_timer_ready(t);   /* paint the first frame immediately */
}

#if CONFIG_LCD_LVGL_BENCHMARK
/* Full-screen refresh benchmark: force whole-screen redraws and measure
 * frames-per-second (render + SPI transfer) plus the raw SPI flush
 * throughput to the panel. */
static void run_fullscreen_benchmark(void)
{
    const int frames = 30;
    lv_obj_t *scr = lv_scr_act();

    /* Warm-up: one full frame so renderer/caches are ready. */
    lv_obj_invalidate(scr);
    lv_timer_handler();

    const uint32_t flushes0 = s_flush_count;
    const uint64_t flush_us0 = s_flush_us;
    const uint64_t flush_bytes0 = s_flush_bytes;

    lv_timer_t *refr = lv_display_get_refr_timer(lv_display_get_default());

    uint64_t render_us = 0;
    const uint64_t t0 = esp_timer_get_time();
    for (int i = 0; i < frames; i++) {
        /* Cycle the full-screen background so every pixel is rewritten. */
        const uint32_t color = 0x000000u | ((uint32_t)(i & 7) << 5);
        lv_obj_set_style_bg_color(scr, lv_color_hex(color), 0);
        lv_obj_invalidate(scr);
        /* LVGL 9: refresh timer pauses itself after each run; force it due
         * so every loop iteration really redraws the whole screen. */
        lv_timer_ready(refr);
        const uint64_t r0 = esp_timer_get_time();
        lv_timer_handler();
        render_us += esp_timer_get_time() - r0;
    }
    const uint64_t t1 = esp_timer_get_time();

    const uint64_t elapsed_us = (uint64_t)(t1 - t0);
    const double frame_ms = (double)elapsed_us / 1000.0 / (double)frames;
    const double fps = 1000.0 / frame_ms;

    const uint32_t flushes = s_flush_count - flushes0;
    const uint64_t flush_us = s_flush_us - flush_us0;
    const uint64_t flush_bytes = s_flush_bytes - flush_bytes0;
    const double mbps = (double)flush_bytes / ((double)flush_us / 1e6) / 1e6;

    ESP_LOGI(TAG, "FULLSCREEN render: %.2f ms/frame (timer_handler), flush: %.2f ms/frame",
             (double)render_us / 1000.0 / (double)frames,
             (double)flush_us / 1000.0 / (double)frames);
    ESP_LOGI(TAG, "FULLSCREEN refresh: %.2f fps (%.3f ms/frame, %d frames, %dx%d)",
             fps, frame_ms, frames, LCD_H_RES, LCD_V_RES);
    ESP_LOGI(TAG, "FULLSCREEN flush: %u partial flushes (%llu B), %.2f MB/s to panel",
             flushes, (unsigned long long)flush_bytes, mbps);
}
#endif /* CONFIG_LCD_LVGL_BENCHMARK */

/* ═══════════════ FT5x06 电容触摸（与 LCD 同 I2C 总线） ═══════════════ */

static esp_lcd_touch_handle_t s_touch = NULL;

/* LVGL 轮询读取触摸点（LV_OS_NONE：read_cb 运行在 display 任务上下文，天然线程安全） */
static void touch_read_cb(lv_indev_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    if (s_touch == NULL) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }
    esp_lcd_touch_read_data(s_touch);
    esp_lcd_touch_point_data_t pts[1];
    uint8_t n = 0;
    if (esp_lcd_touch_get_data(s_touch, pts, &n, 1) == ESP_OK && n > 0) {
        data->point.x = pts[0].x;
        data->point.y = pts[0].y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

/* 初始化触摸屏并注册 LVGL 指针输入设备；坐标映射与屏幕旋转（swap_xy + mirror_x）一致 */
static void bsp_touch_init(void)
{
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_V_RES,      /* 触控 IC 原生 240x320，经 swap+mirror 映射到 320x240 */
        .y_max = LCD_H_RES,
        .rst_gpio_num = GPIO_NUM_NC,  /* 与 LCD 共用复位 */
        .int_gpio_num = GPIO_NUM_NC,  /* 轮询模式，不用中断脚 */
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 1,
            .mirror_x = 1,
            .mirror_y = 0,
        },
    };

    esp_lcd_panel_io_handle_t tp_io = NULL;
    const esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    if (esp_lcd_new_panel_io_i2c(s_i2c_bus, &tp_io_cfg, &tp_io) != ESP_OK) {
        ESP_LOGE(TAG, "touch panel IO init failed");
        return;
    }
    if (esp_lcd_touch_new_i2c_ft5x06(tp_io, &tp_cfg, &s_touch) != ESP_OK) {
        ESP_LOGE(TAG, "FT5x06 touch init failed");
        return;
    }

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
    ESP_LOGI(TAG, "FT5x06 touch ready, LVGL pointer indev registered");
}
static void display_task(void *arg)
{
    (void)arg;

    esp_err_t err = lcd_panel_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LCD init failed (%s); display task stopped",
                 esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    /* Two full-frame draw buffers; location is a Kconfig switch
     * (LCD_LVGL_BUF_IN_PSRAM) so DRAM vs PSRAM refresh can be compared. */
#if CONFIG_LCD_LVGL_BUF_IN_PSRAM
    uint32_t buf_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#else
    uint32_t buf_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#endif
    uint8_t *buf1 = heap_caps_malloc(LCD_BUFFER_BYTES, buf_caps);
    uint8_t *buf2 = heap_caps_malloc(LCD_BUFFER_BYTES, buf_caps);
    if (buf1 == NULL || buf2 == NULL) {
        ESP_LOGE(TAG, "Not enough memory for LVGL draw buffers");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "LVGL buffers in %s: buf1=%p buf2=%p (%u B each)",
             (buf_caps & MALLOC_CAP_SPIRAM) ? "PSRAM" : "DRAM",
             buf1, buf2, (unsigned)LCD_BUFFER_BYTES);

    lv_init();
    lv_tick_set_cb(lv_tick_get_ms);

    lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    /* Render directly in ST7789-native byte order (big-endian RGB565):
     * removes the per-pixel byte-swap loop in the flush callback. */
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_flush_cb(disp, lcd_flush_cb);
    s_flush_done = xSemaphoreCreateBinary();
    if (s_flush_done == NULL) {
        ESP_LOGE(TAG, "Failed to create flush semaphore");
    }
    lv_display_set_flush_wait_cb(disp, lcd_flush_wait_cb);

    /* Flush is async: only release the LVGL draw buffer when the transfer
     * has actually finished (on_color_trans_done), never before. */
    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = lcd_flush_ready_cb,
    };
    esp_err_t cb_err = esp_lcd_panel_io_register_event_callbacks(s_io, &cbs, disp);
    if (cb_err != ESP_OK) {
        ESP_LOGE(TAG, "Register panel-io callback failed: %s",
                 esp_err_to_name(cb_err));
    }

    lv_display_set_buffers(disp, buf1, buf2, LCD_BUFFER_BYTES,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    bsp_touch_init();   /* 触摸屏 + LVGL 指针输入设备 */


    demo_screen_create();

    ESP_LOGI(TAG, "display task started");
#if CONFIG_LCD_LVGL_BENCHMARK
    raw_spi_benchmark();
    run_fullscreen_benchmark();
#endif
    while (1) {
        uint32_t time_till_next = lv_timer_handler();
        /* Sleep until the next LVGL timer is due instead of polling every
         * DISPLAY_REFRESH_MS. lv_timer_handler() returns ms until the next
         * due timer; cap it so we stay responsive. */
        if (time_till_next > DISPLAY_REFRESH_MS) {
            time_till_next = DISPLAY_REFRESH_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(time_till_next));
    }
}

/* ═══════════════ public API ═══════════════ */

esp_err_t lcd_lvgl_get_i2c_bus(i2c_master_bus_handle_t *bus)
{
    if (bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *bus = s_i2c_bus;
    return (s_i2c_bus != NULL) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void lcd_lvgl_set_screen_builder(lcd_lvgl_screen_builder_t builder)
{
    s_screen_builder = builder;
}

esp_err_t lcd_lvgl_start(void)
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "I2C init failed");
    ESP_RETURN_ON_ERROR(pca9557_init(), TAG, "PCA9557 init failed");
    ESP_RETURN_ON_ERROR(bsp_display_backlight_init(), TAG, "Backlight init failed");

    if (xTaskCreate(display_task, "display", DISPLAY_TASK_STACK_BYTES, NULL,
                    DISPLAY_TASK_PRIORITY, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "display task created");
    return ESP_OK;
}
