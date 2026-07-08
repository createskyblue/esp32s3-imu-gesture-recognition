#include "esp_err.h"
#include "esp_log.h"

#include <stdlib.h>
#include <time.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_task.h"
#include "sd_card.h"
#include "sd_logger.h"
#include "web_server.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    /* Set timezone to UTC+8 (China Standard Time) */
    setenv("TZ", "CST-8", 1);
    tzset();

    ESP_ERROR_CHECK(led_task_init());

    /* SD card is optional — log error but don't halt */
    esp_err_t sd_err = sd_card_init();
    if (sd_err != ESP_OK) {
        ESP_LOGW(TAG, "SD card init failed: %s (continuing without SD)", esp_err_to_name(sd_err));
    } else {
        esp_err_t log_err = sd_logger_init();
        if (log_err != ESP_OK) {
            ESP_LOGW(TAG, "SD logger init failed: %s", esp_err_to_name(log_err));
        }
    }

    ESP_ERROR_CHECK(web_server_init_and_start());
    ESP_LOGI(TAG, "all tasks started");
}

/* ── 空闲钩子：绿灯系统存活指示 ────────────────────────────────────── */
void vApplicationIdleHook(void)
{
    static TickType_t last_toggle_tick = 0;
    static bool led_state = false;

    const TickType_t now = xTaskGetTickCount();
    if ((now - last_toggle_tick) >= pdMS_TO_TICKS(500)) {
        led_state = !led_state;
        gpio_set_level(GPIO_NUM_6, led_state ? 0 : 1);  /* 低电平激活 */
        last_toggle_tick = now;
    }
}
