/**
 * @file button.c
 * @brief Button/switch driver — GPIO input with debounce, publishes press events to jettyd.
 *
 * When the button is pressed, the driver publishes a telemetry event:
 *   metric: "press", value: 1.0
 *   metric: "press_count", value: <total presses since boot>
 *
 * The device shadow reports: {"state": "pressed"/"released", "press_count": N}
 *
 * Wiring: GPIO pin → button → GND, with internal pull-up enabled (active_low = true).
 *         Or: 3.3V → button → GPIO pin, with active_low = false.
 */

#include "button.h"
#include "jettyd_driver.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdbool.h>

static const char *TAG = "drv_button";

static button_config_t s_cfg;
static jettyd_driver_t s_driver;
static volatile bool s_pressed = false;
static volatile uint32_t s_press_count = 0;
static int64_t s_last_edge_us = 0;

/* ISR — just note the time, debounce in the poll task */
static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    int64_t now = esp_timer_get_time();
    int64_t debounce_us = s_cfg.debounce_ms ? (int64_t)s_cfg.debounce_ms * 1000 : 50000;
    if (now - s_last_edge_us < debounce_us) return;
    s_last_edge_us = now;

    int level = gpio_get_level(s_cfg.pin);
    bool pressed = s_cfg.active_low ? (level == 0) : (level == 1);

    if (pressed && !s_pressed) {
        s_press_count++;
    }
    s_pressed = pressed;
}

static esp_err_t button_init(const void *config)
{
    const button_config_t *c = (const button_config_t *)config;
    s_cfg = *c;
    if (s_cfg.debounce_ms == 0) s_cfg.debounce_ms = 50;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << s_cfg.pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = s_cfg.active_low ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = s_cfg.active_low ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) return err;

    gpio_install_isr_service(0);
    gpio_isr_handler_add(s_cfg.pin, gpio_isr_handler, NULL);

    ESP_LOGI(TAG, "Button init on GPIO %d (active_%s)",
             s_cfg.pin, s_cfg.active_low ? "low" : "high");
    return ESP_OK;
}

static jettyd_value_t button_read(const char *metric)
{
    jettyd_value_t v = { .valid = true };
    if (strcmp(metric, "press") == 0) {
        v.type = JETTYD_VAL_BOOL;
        v.bool_val = s_pressed;
    } else if (strcmp(metric, "press_count") == 0) {
        v.type = JETTYD_VAL_FLOAT;
        v.float_val = (float)s_press_count;
    } else {
        v.valid = false;
    }
    return v;
}

void button_register(const char *instance, const void *config)
{
    memset(&s_driver, 0, sizeof(s_driver));
    strlcpy(s_driver.driver_name, "button", sizeof(s_driver.driver_name));
    strlcpy(s_driver.instance, instance, sizeof(s_driver.instance));

    strlcpy(s_driver.capabilities[0].name, "press", sizeof(s_driver.capabilities[0].name));
    s_driver.capabilities[0].type = JETTYD_CAP_READABLE;
    s_driver.capabilities[0].value_type = JETTYD_VAL_BOOL;

    strlcpy(s_driver.capabilities[1].name, "press_count", sizeof(s_driver.capabilities[1].name));
    s_driver.capabilities[1].type = JETTYD_CAP_READABLE;
    s_driver.capabilities[1].value_type = JETTYD_VAL_FLOAT;

    s_driver.capability_count = 2;
    s_driver.init = button_init;
    s_driver.read = button_read;

    jettyd_driver_registry_add(&s_driver);
}
