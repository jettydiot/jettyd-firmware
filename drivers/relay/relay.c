/**
 * @file relay.c
 * @brief Relay driver — GPIO-based switchable actuator with safety timer.
 */

#include "relay.h"
#include "jettyd_driver.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <stdbool.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include <string.h>

static const char *TAG = "drv_relay";

static relay_config_t s_cfg;
static jettyd_driver_t s_driver;
static bool s_state = false;
static TimerHandle_t s_auto_off_timer = NULL;

static void set_gpio(bool on)
{
    uint32_t level = on ? (s_cfg.active_high ? 1 : 0) : (s_cfg.active_high ? 0 : 1);
    gpio_set_level(s_cfg.pin, level);
    s_state = on;
}

static void auto_off_callback(TimerHandle_t timer)
{
    ESP_LOGW(TAG, "Safety auto-off triggered for pin %d", s_cfg.pin);
    set_gpio(false);
}

static esp_err_t relay_init(const void *config)
{
    const relay_config_t *c = (const relay_config_t *)config;
    s_cfg = *c;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << s_cfg.pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed: %s", esp_err_to_name(err));
        return err;
    }

    set_gpio(s_cfg.default_state_on);

    /* Create auto-off timer (one-shot, not started) */
    s_auto_off_timer = xTimerCreate("relay_off", pdMS_TO_TICKS(1000),
                                     pdFALSE, NULL, auto_off_callback);

    ESP_LOGI(TAG, "Relay init: pin=%d, active_high=%d, max_on=%lu s",
             s_cfg.pin, s_cfg.active_high, (unsigned long)s_cfg.max_on_duration);
    return ESP_OK;
}

static esp_err_t relay_deinit(void)
{
    set_gpio(false);
    if (s_auto_off_timer) {
        xTimerDelete(s_auto_off_timer, 0);
        s_auto_off_timer = NULL;
    }
    return ESP_OK;
}

static jettyd_value_t relay_read(const char *capability)
{
    jettyd_value_t val = {
        .type = JETTYD_VAL_BOOL,
        .bool_val = s_state,
        .valid = true,
    };
    return val;
}

static esp_err_t relay_switch_on(uint32_t duration_ms)
{
    set_gpio(true);
    ESP_LOGI(TAG, "Relay ON (pin %d)", s_cfg.pin);

    /* Determine auto-off duration */
    uint32_t max_ms = s_cfg.max_on_duration * 1000;
    uint32_t off_ms = 0;

    if (duration_ms > 0 && max_ms > 0) {
        off_ms = (duration_ms < max_ms) ? duration_ms : max_ms;
    } else if (duration_ms > 0) {
        off_ms = duration_ms;
    } else if (max_ms > 0) {
        off_ms = max_ms;
    }

    if (off_ms > 0 && s_auto_off_timer) {
        xTimerChangePeriod(s_auto_off_timer, pdMS_TO_TICKS(off_ms), 0);
        xTimerStart(s_auto_off_timer, 0);
        ESP_LOGI(TAG, "Auto-off in %lu ms", (unsigned long)off_ms);
    }

    return ESP_OK;
}

static esp_err_t relay_switch_off(void)
{
    set_gpio(false);
    ESP_LOGI(TAG, "Relay OFF (pin %d)", s_cfg.pin);

    if (s_auto_off_timer) {
        xTimerStop(s_auto_off_timer, 0);
    }

    return ESP_OK;
}

static bool relay_get_state(void)
{
    return s_state;
}

static esp_err_t relay_self_test(void)
{
    /* Toggle relay briefly to verify it works */
    bool original = s_state;
    set_gpio(!original);
    vTaskDelay(pdMS_TO_TICKS(50));
    set_gpio(original);
    return ESP_OK;
}

void relay_register(const char *instance, const void *config)
{
    relay_init(config);

    memset(&s_driver, 0, sizeof(s_driver));
    strncpy(s_driver.instance, instance, JETTYD_MAX_INSTANCE_NAME - 1);
    strncpy(s_driver.driver_name, "relay", sizeof(s_driver.driver_name) - 1);

    s_driver.capability_count = 1;
    strncpy(s_driver.capabilities[0].name, "state", sizeof(s_driver.capabilities[0].name) - 1);
    s_driver.capabilities[0].type = JETTYD_CAP_SWITCHABLE;
    s_driver.capabilities[0].value_type = JETTYD_VAL_BOOL;
    s_driver.capabilities[0].min_value = 0;
    s_driver.capabilities[0].max_value = 1;

    s_driver.init = relay_init;
    s_driver.deinit = relay_deinit;
    s_driver.read = relay_read;
    s_driver.switch_on = relay_switch_on;
    s_driver.switch_off = relay_switch_off;
    s_driver.get_state = relay_get_state;
    s_driver.self_test = relay_self_test;

    JETTYD_REGISTER_DRIVER(&s_driver);
}
