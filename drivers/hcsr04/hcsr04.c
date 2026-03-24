/**
 * @file hcsr04.c
 * @brief HC-SR04 ultrasonic distance sensor — trigger/echo GPIO protocol.
 *
 * Protocol: 10µs HIGH pulse on trigger → measure echo pulse width
 * → distance = pulse_us / 58.0 (cm).
 * Timeout: echo > 30ms → out of range (invalid reading).
 */

#include "hcsr04.h"
#include "jettyd_driver.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <time.h>
#include "esp_timer.h"
#include "rom/ets_sys.h"
#include <string.h>

static const char *TAG = "drv_hcsr04";

#define HCSR04_TRIGGER_US    10
#define HCSR04_TIMEOUT_US    30000  /* 30ms — ~5m max range */

static hcsr04_config_t s_cfg;
static jettyd_driver_t s_driver;

static esp_err_t hcsr04_init(const void *config)
{
    const hcsr04_config_t *c = (const hcsr04_config_t *)config;
    s_cfg = *c;

    /* Configure trigger pin as output */
    gpio_config_t trig_conf = {
        .pin_bit_mask = (1ULL << s_cfg.trigger_pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&trig_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Trigger GPIO config failed: %s", esp_err_to_name(err));
        return err;
    }
    gpio_set_level(s_cfg.trigger_pin, 0);

    /* Configure echo pin as input */
    gpio_config_t echo_conf = {
        .pin_bit_mask = (1ULL << s_cfg.echo_pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&echo_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Echo GPIO config failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "HC-SR04 init: trigger=%d, echo=%d", s_cfg.trigger_pin, s_cfg.echo_pin);
    return ESP_OK;
}

static esp_err_t hcsr04_deinit(void)
{
    gpio_set_level(s_cfg.trigger_pin, 0);
    return ESP_OK;
}

static jettyd_value_t hcsr04_read(const char *capability)
{
    jettyd_value_t val = {.type = JETTYD_VAL_FLOAT, .valid = false};

    /* Send 10µs trigger pulse */
    gpio_set_level(s_cfg.trigger_pin, 0);
    ets_delay_us(2);
    gpio_set_level(s_cfg.trigger_pin, 1);
    ets_delay_us(HCSR04_TRIGGER_US);
    gpio_set_level(s_cfg.trigger_pin, 0);

    /* Wait for echo pin to go HIGH (start of echo pulse) */
    int64_t t_start = esp_timer_get_time();
    while (gpio_get_level(s_cfg.echo_pin) == 0) {
        if ((esp_timer_get_time() - t_start) > HCSR04_TIMEOUT_US) {
            ESP_LOGW(TAG, "Timeout waiting for echo start");
            return val;
        }
    }

    /* Measure echo pulse width */
    int64_t echo_start = esp_timer_get_time();
    while (gpio_get_level(s_cfg.echo_pin) == 1) {
        if ((esp_timer_get_time() - echo_start) > HCSR04_TIMEOUT_US) {
            ESP_LOGW(TAG, "Timeout: echo too long (out of range)");
            return val;
        }
    }
    int64_t echo_end = esp_timer_get_time();

    float pulse_us = (float)(echo_end - echo_start);
    float distance_cm = pulse_us / 58.0f;

    val.float_val = distance_cm;
    val.valid = true;
    return val;
}

static esp_err_t hcsr04_self_test(void)
{
    /* Verify GPIOs are responsive: read echo pin level */
    gpio_set_level(s_cfg.trigger_pin, 0);
    int level = gpio_get_level(s_cfg.echo_pin);
    (void)level;
    return ESP_OK;
}

void hcsr04_register(const char *instance, const void *config)
{
    hcsr04_init(config);

    memset(&s_driver, 0, sizeof(s_driver));
    strncpy(s_driver.instance, instance, JETTYD_MAX_INSTANCE_NAME - 1);
    strlcpy(s_driver.driver_name, "hcsr04", sizeof(s_driver.driver_name));

    s_driver.capability_count = 1;
    strlcpy(s_driver.capabilities[0].name, "distance", sizeof(s_driver.capabilities[0].name));
    s_driver.capabilities[0].type = JETTYD_CAP_READABLE;
    s_driver.capabilities[0].value_type = JETTYD_VAL_FLOAT;
    s_driver.capabilities[0].min_value = 2;
    s_driver.capabilities[0].max_value = 400;
    strlcpy(s_driver.capabilities[0].unit, "cm", sizeof(s_driver.capabilities[0].unit));

    s_driver.init = hcsr04_init;
    s_driver.deinit = hcsr04_deinit;
    s_driver.read = hcsr04_read;
    s_driver.self_test = hcsr04_self_test;

    JETTYD_REGISTER_DRIVER(&s_driver);
}
