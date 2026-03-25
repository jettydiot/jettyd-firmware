/**
 * @file led.c
 * @brief LED driver — GPIO output with on/off/blink control via jettyd commands.
 *
 * Commands accepted (via MQTT command topic):
 *   {"action": "led.on"}
 *   {"action": "led.off"}
 *   {"action": "led.blink", "params": {"interval_ms": 500, "count": 3}}
 *   {"action": "led.toggle"}
 *
 * Shadow reports: {"state": "on"/"off", "blink": false}
 */

#include "led.h"
#include "jettyd_driver.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include <string.h>
#include <stdbool.h>

static const char *TAG = "drv_led";

static led_config_t s_cfg;
static jettyd_driver_t s_driver;
static bool s_state = false;
static TimerHandle_t s_blink_timer = NULL;
static int s_blink_remaining = 0;

static void set_gpio(bool on)
{
    uint32_t level = on ? (s_cfg.active_high ? 1 : 0) : (s_cfg.active_high ? 0 : 1);
    gpio_set_level(s_cfg.pin, level);
    s_state = on;
}

static void blink_callback(TimerHandle_t timer)
{
    set_gpio(!s_state);
    if (s_blink_remaining > 0) {
        s_blink_remaining--;
        if (s_blink_remaining == 0) {
            xTimerStop(s_blink_timer, 0);
            set_gpio(false);
        }
    }
}

static esp_err_t led_init(const void *config)
{
    const led_config_t *c = (const led_config_t *)config;
    s_cfg = *c;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << s_cfg.pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) return err;

    set_gpio(false);

    s_blink_timer = xTimerCreate("led_blink", pdMS_TO_TICKS(500),
                                  pdTRUE, NULL, blink_callback);
    ESP_LOGI(TAG, "LED init on GPIO %d", s_cfg.pin);
    return ESP_OK;
}

static esp_err_t led_switch_on(uint32_t duration_ms)
{
    if (s_blink_timer) xTimerStop(s_blink_timer, 0);
    set_gpio(true);
    (void)duration_ms; /* TODO: auto-off timer */
    return ESP_OK;
}

static esp_err_t led_switch_off(void)
{
    if (s_blink_timer) xTimerStop(s_blink_timer, 0);
    set_gpio(false);
    return ESP_OK;
}

static jettyd_value_t led_read(const char *metric)
{
    jettyd_value_t v = { .type = JETTYD_VAL_BOOL, .valid = true };
    v.bool_val = s_state;
    return v;
}

static esp_err_t led_command(const char *action, const char *params_json)
{
    if (strcmp(action, "led.on") == 0) {
        return led_switch_on();
    } else if (strcmp(action, "led.off") == 0) {
        return led_switch_off();
    } else if (strcmp(action, "led.toggle") == 0) {
        return s_state ? led_switch_off() : led_switch_on();
    } else if (strcmp(action, "led.blink") == 0) {
        /* Parse interval_ms and count from params — use defaults if absent */
        int interval_ms = 500;
        int count = 6; /* 3 blinks = 6 toggles */

        if (params_json) {
            /* Simple extraction — avoid heap allocation */
            const char *p = strstr(params_json, "interval_ms");
            if (p) interval_ms = atoi(p + 13);
            p = strstr(params_json, "count");
            if (p) count = atoi(p + 7) * 2; /* each blink = 2 toggles */
        }

        if (s_blink_timer) {
            xTimerChangePeriod(s_blink_timer, pdMS_TO_TICKS(interval_ms), 0);
            s_blink_remaining = count;
            set_gpio(true);
            xTimerStart(s_blink_timer, 0);
        }
        return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

void led_register(const char *instance, const void *config)
{
    memset(&s_driver, 0, sizeof(s_driver));
    strlcpy(s_driver.driver_name, "led", sizeof(s_driver.driver_name));
    strlcpy(s_driver.instance, instance, sizeof(s_driver.instance));

    s_driver.capabilities[0].name[0] = '\0';
    strlcpy(s_driver.capabilities[0].name, "state", sizeof(s_driver.capabilities[0].name));
    s_driver.capabilities[0].type = JETTYD_CAP_SWITCHABLE;
    s_driver.capabilities[0].value_type = JETTYD_VAL_BOOL;
    s_driver.capability_count = 1;

    s_driver.init       = led_init;
    s_driver.read       = led_read;
    s_driver.switch_on  = led_switch_on;
    s_driver.switch_off = led_switch_off;

    jettyd_driver_registry_add(&s_driver);
}
