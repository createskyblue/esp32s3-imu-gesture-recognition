#pragma once

#include "esp_err.h"
#include <stdint.h>

typedef enum {
    LED_RED = 0,
    LED_YELLOW,
    LED_GREEN,
    LED_BLUE,
    LED_COUNT,
} led_id_t;

typedef enum {
    LED_CMD_OFF = 0,
    LED_CMD_ON,
    LED_CMD_BLINK,
    LED_CMD_ONESHOT,
} led_cmd_type_t;

typedef struct {
    led_id_t led;
    led_cmd_type_t type;
    uint32_t period_ms;
    uint32_t on_ms;
} led_cmd_t;

/** Configure all four LED GPIOs and start the command task. */
esp_err_t led_task_init(void);

/** Queue a command after led_task_init(); commands are ignored before init. */
void led_send_cmd(const led_cmd_t *cmd);

/** Immediately turn on the red LED (call from fatal-error paths before reboot). */
void led_fatal_error(void);
