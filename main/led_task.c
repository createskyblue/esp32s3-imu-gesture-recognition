#include "led_task.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define LED_RED_GPIO    GPIO_NUM_15
#define LED_YELLOW_GPIO GPIO_NUM_7
#define LED_GREEN_GPIO  GPIO_NUM_6
#define LED_BLUE_GPIO   GPIO_NUM_5

#define LED_TASK_STACK_BYTES  2048u
#define LED_TASK_PRIORITY     5u
#define LED_QUEUE_LENGTH      16u

#define LED_ACTIVE_LEVEL      0u
#define LED_INACTIVE_LEVEL    1u

#define LED_THROTTLE_MS       200u

static const char *TAG = "LED";

static const gpio_num_t s_gpio[LED_COUNT] = {
    [LED_RED]    = LED_RED_GPIO,
    [LED_YELLOW] = LED_YELLOW_GPIO,
    [LED_GREEN]  = LED_GREEN_GPIO,
    [LED_BLUE]   = LED_BLUE_GPIO,
};

typedef enum {
    MODE_OFF = 0,
    MODE_ON,
    MODE_BLINK,
} led_mode_t;

typedef struct {
    led_mode_t mode;        /* persistent mode: OFF / ON / BLINK */
    uint32_t period_ms;     /* blink period */
    uint32_t on_ms;         /* blink on-duration */
    TickType_t deadline;    /* next toggle time for blink */
    bool lit;               /* current blink state */
    bool flash_active;      /* ONESHOT flash overlay active */
    TickType_t flash_deadline; /* when the flash expires */
} led_state_t;

static QueueHandle_t s_queue;
static led_state_t s_state[LED_COUNT];
static TickType_t s_last_cmd_time[LED_COUNT];

static void led_set_level(led_id_t led, bool on)
{
    gpio_set_level(s_gpio[led], on ? LED_ACTIVE_LEVEL : LED_INACTIVE_LEVEL);
}

static void led_apply(led_id_t led)
{
    if (s_state[led].flash_active) {
        led_set_level(led, true);
        return;
    }
    switch (s_state[led].mode) {
    case MODE_OFF:
        led_set_level(led, false);
        break;
    case MODE_ON:
        led_set_level(led, true);
        break;
    case MODE_BLINK:
        led_set_level(led, s_state[led].lit);
        break;
    default:
        break;
    }
}

static void led_process_cmd(const led_cmd_t *cmd)
{
    const led_id_t led = cmd->led;
    if (led >= LED_COUNT) {
        return;
    }

    switch (cmd->type) {
    case LED_CMD_OFF:
        s_state[led].mode = MODE_OFF;
        s_state[led].flash_active = false;
        led_set_level(led, false);
        break;

    case LED_CMD_ON:
        s_state[led].mode = MODE_ON;
        s_state[led].flash_active = false;
        led_set_level(led, true);
        break;

    case LED_CMD_BLINK:
        s_state[led].mode = MODE_BLINK;
        s_state[led].period_ms = cmd->period_ms;
        s_state[led].on_ms = cmd->on_ms;
        s_state[led].lit = true;
        s_state[led].deadline = xTaskGetTickCount() + pdMS_TO_TICKS(cmd->on_ms);
        s_state[led].flash_active = false;
        led_set_level(led, true);
        break;

    case LED_CMD_ONESHOT:
        s_state[led].flash_active = true;
        s_state[led].flash_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(cmd->on_ms);
        led_set_level(led, true);
        break;
    }
}

static TickType_t led_update(void)
{
    const TickType_t now = xTaskGetTickCount();
    TickType_t earliest = portMAX_DELAY;

    for (int led = 0; led < LED_COUNT; ++led) {
        /* flash overlay: check expiry first */
        if (s_state[led].flash_active) {
            if ((TickType_t)(now - s_state[led].flash_deadline) < 0x80000000u) {
                s_state[led].flash_active = false;
                led_apply((led_id_t)led); /* restore base mode */
            } else {
                const TickType_t remaining = s_state[led].flash_deadline - now;
                if (remaining < earliest) {
                    earliest = remaining;
                }
            }
        }

        /* blink mode: toggle when deadline reached */
        if (s_state[led].mode == MODE_BLINK && !s_state[led].flash_active) {
            if ((TickType_t)(now - s_state[led].deadline) < 0x80000000u) {
                s_state[led].lit = !s_state[led].lit;
                led_apply((led_id_t)led);
                const uint32_t next_ms = s_state[led].lit
                    ? s_state[led].on_ms
                    : (s_state[led].period_ms > s_state[led].on_ms
                           ? s_state[led].period_ms - s_state[led].on_ms
                           : 0u);
                s_state[led].deadline = now + pdMS_TO_TICKS(next_ms);
            }

            const TickType_t remaining = s_state[led].deadline - now;
            if (remaining < earliest) {
                earliest = remaining;
            }
        }
    }

    return earliest;
}

static void led_task(void *arg)
{
    (void)arg;
    led_cmd_t cmd;

    while (1) {
        TickType_t wait = led_update();
        if (xQueueReceive(s_queue, &cmd, wait) == pdTRUE) {
            led_process_cmd(&cmd);
        }
    }
}

void led_send_cmd(const led_cmd_t *cmd)
{
    if (s_queue == NULL || cmd == NULL ||
        cmd->led == LED_GREEN || cmd->led >= LED_COUNT) {
        return;
    }
    const TickType_t now = xTaskGetTickCount();
    if (cmd->led != LED_RED && s_last_cmd_time[cmd->led] != 0 &&
        pdTICKS_TO_MS(now - s_last_cmd_time[cmd->led]) < LED_THROTTLE_MS) {
        return;
    }
    s_last_cmd_time[cmd->led] = now;
    xQueueSend(s_queue, cmd, 0);
}

void led_fatal_error(void)
{
    gpio_set_level(LED_RED_GPIO, LED_ACTIVE_LEVEL);
    if (s_queue != NULL) {
        const led_cmd_t cmd = { .led = LED_RED, .type = LED_CMD_ON };
        xQueueSend(s_queue, &cmd, 0);
    }
}

esp_err_t led_task_init(void)
{
    /* 绿灯：仅空闲钩子直控，不参与队列管理 */
    {
        gpio_config_t conf = {
            .pin_bit_mask = (1ULL << LED_GREEN_GPIO),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&conf));
        gpio_set_level(LED_GREEN_GPIO, LED_INACTIVE_LEVEL);
    }

    /* 红灯、黄灯、蓝灯：队列管理 */
    for (int i = 0; i < LED_COUNT; ++i) {
        if (i == LED_GREEN) {
            continue;
        }
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << s_gpio[i]),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&io_conf));
        gpio_set_level(s_gpio[i], LED_INACTIVE_LEVEL);
        s_state[i].mode = MODE_OFF;
        s_state[i].flash_active = false;
        s_last_cmd_time[i] = 0;
    }

    s_queue = xQueueCreate(LED_QUEUE_LENGTH, sizeof(led_cmd_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    xTaskCreate(led_task, "led", LED_TASK_STACK_BYTES, NULL, LED_TASK_PRIORITY, NULL);
    ESP_LOGI(TAG, "LED task started (R=%d Y=%d G=%d B=%d)",
             LED_RED_GPIO, LED_YELLOW_GPIO, LED_GREEN_GPIO, LED_BLUE_GPIO);
    return ESP_OK;
}
