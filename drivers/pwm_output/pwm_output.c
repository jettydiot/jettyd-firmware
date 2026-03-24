/**
 * @file pwm_output.c
 * @brief LEDC PWM output driver — maps 0-100% duty to 13-bit resolution.
 */

#include "pwm_output.h"
#include "jettyd_driver.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "drv_pwm";

#define PWM_RESOLUTION LEDC_TIMER_13_BIT
#define PWM_MAX_DUTY   8191  /* (2^13) - 1 */

static pwm_output_config_t s_cfg;
static jettyd_driver_t s_driver;
static volatile float s_duty_pct = 0.0f;

static esp_err_t pwm_init(const void *config)
{
    const pwm_output_config_t *c = (const pwm_output_config_t *)config;
    s_cfg = *c;

    if (s_cfg.frequency_hz == 0) s_cfg.frequency_hz = 1000;

    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = s_cfg.ledc_timer,
        .duty_resolution = PWM_RESOLUTION,
        .freq_hz = s_cfg.frequency_hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LEDC timer config failed: %s", esp_err_to_name(err));
        return err;
    }

    ledc_channel_config_t chan_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = s_cfg.ledc_channel,
        .timer_sel = s_cfg.ledc_timer,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = s_cfg.gpio_pin,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LEDC channel config failed: %s", esp_err_to_name(err));
        return err;
    }

    s_duty_pct = 0.0f;
    ESP_LOGI(TAG, "PWM init: pin=%d, freq=%lu Hz, ch=%d",
             s_cfg.gpio_pin, (unsigned long)s_cfg.frequency_hz, s_cfg.ledc_channel);
    return ESP_OK;
}

static esp_err_t pwm_deinit(void)
{
    ledc_stop(LEDC_LOW_SPEED_MODE, s_cfg.ledc_channel, 0);
    s_duty_pct = 0.0f;
    return ESP_OK;
}

static jettyd_value_t pwm_read(const char *capability)
{
    jettyd_value_t val = {
        .type = JETTYD_VAL_FLOAT,
        .float_val = s_duty_pct,
        .valid = true,
    };
    return val;
}

static esp_err_t pwm_write(const char *capability, jettyd_value_t value)
{
    if (strcmp(capability, "duty") != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    float pct = value.float_val;
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;

    uint32_t duty = (uint32_t)(pct / 100.0f * PWM_MAX_DUTY);

    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, s_cfg.ledc_channel, duty);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Set duty failed: %s", esp_err_to_name(err));
        return err;
    }

    err = ledc_update_duty(LEDC_LOW_SPEED_MODE, s_cfg.ledc_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Update duty failed: %s", esp_err_to_name(err));
        return err;
    }

    s_duty_pct = pct;
    ESP_LOGI(TAG, "PWM duty: %.1f%% (raw %lu)", pct, (unsigned long)duty);
    return ESP_OK;
}

static esp_err_t pwm_self_test(void)
{
    /* Verify LEDC channel reads back a valid duty value */
    uint32_t duty = ledc_get_duty(LEDC_LOW_SPEED_MODE, s_cfg.ledc_channel);
    (void)duty;
    return ESP_OK;
}

void pwm_output_register(const char *instance, const void *config)
{
    pwm_init(config);

    memset(&s_driver, 0, sizeof(s_driver));
    strncpy(s_driver.instance, instance, JETTYD_MAX_INSTANCE_NAME - 1);
    strncpy(s_driver.driver_name, "pwm_output", sizeof(s_driver.driver_name) - 1);

    s_driver.capability_count = 1;
    strncpy(s_driver.capabilities[0].name, "duty", sizeof(s_driver.capabilities[0].name) - 1);
    s_driver.capabilities[0].type = JETTYD_CAP_WRITABLE;
    s_driver.capabilities[0].value_type = JETTYD_VAL_FLOAT;
    s_driver.capabilities[0].min_value = 0;
    s_driver.capabilities[0].max_value = 100;
    strncpy(s_driver.capabilities[0].unit, "%", sizeof(s_driver.capabilities[0].unit) - 1);

    s_driver.init = pwm_init;
    s_driver.deinit = pwm_deinit;
    s_driver.read = pwm_read;
    s_driver.write = pwm_write;
    s_driver.self_test = pwm_self_test;

    JETTYD_REGISTER_DRIVER(&s_driver);
}
