/**
 * @file pwm_output.c
 * @brief PWM output driver — LEDC-based writable actuator with safety auto-off timer.
 */

#include "pwm_output.h"
#include "jettyd_driver.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include <string.h>

static const char *TAG = "drv_pwm";

static pwm_output_config_t s_cfg;
static jettyd_driver_t s_driver;
static float s_duty_pct = 0.0f;
static TimerHandle_t s_auto_off_timer = NULL;

#define PWM_LEDC_TIMER     LEDC_TIMER_0
#define PWM_LEDC_MODE      LEDC_LOW_SPEED_MODE
#define PWM_LEDC_DUTY_RES  LEDC_TIMER_13_BIT
#define PWM_LEDC_MAX_DUTY  ((1 << 13) - 1)

static void set_duty(float pct)
{
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    s_duty_pct = pct;

    uint32_t duty = (uint32_t)((pct / 100.0f) * PWM_LEDC_MAX_DUTY);
    ledc_set_duty(PWM_LEDC_MODE, s_cfg.ledc_channel, duty);
    ledc_update_duty(PWM_LEDC_MODE, s_cfg.ledc_channel);
}

static void auto_off_callback(TimerHandle_t timer)
{
    ESP_LOGW(TAG, "Safety auto-off triggered for PWM pin %d", s_cfg.pin);
    set_duty(0.0f);
}

static esp_err_t pwm_init(const void *config)
{
    const pwm_output_config_t *c = (const pwm_output_config_t *)config;
    s_cfg = *c;

    if (s_cfg.max_on_duration == 0) {
        s_cfg.max_on_duration = 3600;
    }

    ledc_timer_config_t timer_cfg = {
        .speed_mode = PWM_LEDC_MODE,
        .timer_num = PWM_LEDC_TIMER,
        .duty_resolution = PWM_LEDC_DUTY_RES,
        .freq_hz = s_cfg.freq_hz > 0 ? s_cfg.freq_hz : 1000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LEDC timer config failed: %s", esp_err_to_name(err));
        return err;
    }

    ledc_channel_config_t ch_cfg = {
        .speed_mode = PWM_LEDC_MODE,
        .channel = s_cfg.ledc_channel,
        .timer_sel = PWM_LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = s_cfg.pin,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&ch_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LEDC channel config failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Create auto-off timer (one-shot, not started) */
    s_auto_off_timer = xTimerCreate("pwm_off", pdMS_TO_TICKS(1000),
                                     pdFALSE, NULL, auto_off_callback);

    ESP_LOGI(TAG, "PWM init: pin=%d, ch=%d, freq=%lu Hz, max_on=%lu s",
             s_cfg.pin, s_cfg.ledc_channel,
             (unsigned long)timer_cfg.freq_hz, (unsigned long)s_cfg.max_on_duration);
    return ESP_OK;
}

static esp_err_t pwm_deinit(void)
{
    set_duty(0.0f);
    if (s_auto_off_timer) {
        xTimerDelete(s_auto_off_timer, 0);
        s_auto_off_timer = NULL;
    }
    ledc_stop(PWM_LEDC_MODE, s_cfg.ledc_channel, 0);
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
    float pct = 0.0f;
    if (value.type == JETTYD_VAL_FLOAT) {
        pct = value.float_val;
    } else if (value.type == JETTYD_VAL_INT) {
        pct = (float)value.int_val;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    set_duty(pct);
    ESP_LOGI(TAG, "PWM duty set to %.1f%% (pin %d)", pct, s_cfg.pin);

    /* Manage auto-off timer */
    if (s_auto_off_timer) {
        if (pct > 0.0f && s_cfg.max_on_duration > 0) {
            uint32_t off_ms = s_cfg.max_on_duration * 1000;
            xTimerChangePeriod(s_auto_off_timer, pdMS_TO_TICKS(off_ms), 0);
            xTimerStart(s_auto_off_timer, 0);
            ESP_LOGI(TAG, "Auto-off in %lu s", (unsigned long)s_cfg.max_on_duration);
        } else {
            xTimerStop(s_auto_off_timer, 0);
        }
    }

    return ESP_OK;
}

static esp_err_t pwm_self_test(void)
{
    float original = s_duty_pct;
    set_duty(10.0f);
    vTaskDelay(pdMS_TO_TICKS(50));
    set_duty(original);
    return ESP_OK;
}

void pwm_output_register(const char *instance, const void *config)
{
    pwm_init(config);

    memset(&s_driver, 0, sizeof(s_driver));
    strncpy(s_driver.instance, instance, JETTYD_MAX_INSTANCE_NAME - 1);
    strlcpy(s_driver.driver_name, "pwm_output", sizeof(s_driver.driver_name));

    s_driver.capability_count = 1;
    strlcpy(s_driver.capabilities[0].name, "duty", sizeof(s_driver.capabilities[0].name));
    s_driver.capabilities[0].type = JETTYD_CAP_WRITABLE;
    s_driver.capabilities[0].value_type = JETTYD_VAL_FLOAT;
    s_driver.capabilities[0].min_value = 0.0f;
    s_driver.capabilities[0].max_value = 100.0f;
    strlcpy(s_driver.capabilities[0].unit, "%", sizeof(s_driver.capabilities[0].unit));

    s_driver.init = pwm_init;
    s_driver.deinit = pwm_deinit;
    s_driver.read = pwm_read;
    s_driver.write = pwm_write;
    s_driver.self_test = pwm_self_test;

    JETTYD_REGISTER_DRIVER(&s_driver);
}
