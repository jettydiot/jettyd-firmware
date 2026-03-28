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
#include "jettyd_telemetry.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>

static const char *TAG = "drv_button";

static button_config_t s_cfg;
static jettyd_driver_t s_driver;
static volatile bool s_pressed = false;
static volatile uint32_t s_press_count = 0;
static int64_t s_last_edge_us = 0;
static char s_instance[JETTYD_MAX_INSTANCE_NAME];
static uint32_t s_last_reported_count = 0;

/* ISR — just note the time, debounce in the poll task */
static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    (void)arg;
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

/**
 * @brief Event task — polls for new presses and publishes telemetry.
 *
 * Runs at low priority. Wakes every 50 ms, checks if s_press_count has
 * advanced since the last report, and if so logs + publishes telemetry.
 * Kept as a poll loop rather than a notification from the ISR so it stays
 * safe to call FreeRTOS and MQTT APIs without any ISR-safe plumbing.
 */
static void button_event_task(void *arg)
{
    (void)arg;
    char press_metric[48];
    char count_metric[48];
    snprintf(press_metric, sizeof(press_metric), "%s.press", s_instance);
    snprintf(count_metric, sizeof(count_metric), "%s.press_count", s_instance);
    const char *pub_metrics[] = { press_metric, count_metric };

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(50));
        uint32_t count = s_press_count; /* single read — volatile, no mutex needed */
        if (count != s_last_reported_count) {
            s_last_reported_count = count;
            ESP_LOGI(TAG, "Button '%s' pressed (total: %" PRIu32 ")", s_instance, count);
            jettyd_telemetry_publish(pub_metrics, 2);
        }
    }
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

    /* gpio_install_isr_service returns ESP_ERR_INVALID_STATE if already
       installed (e.g. by another driver). That's fine — just continue. */
    esp_err_t isr_err = gpio_install_isr_service(0);
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
        return isr_err;
    }
    gpio_isr_handler_add(s_cfg.pin, gpio_isr_handler, NULL);

    /* Spawn the event reporting task */
    xTaskCreate(button_event_task, "btn_event", 2048, NULL, 5, NULL);

    ESP_LOGI(TAG, "Button init on GPIO %d (active_%s, debounce %"PRIu32"ms)",
             s_cfg.pin, s_cfg.active_low ? "low" : "high", s_cfg.debounce_ms);
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
    strlcpy(s_instance, instance, sizeof(s_instance));

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

    /* Initialise hardware immediately — arms GPIO ISR and starts event task. */
    button_init(config);
}
