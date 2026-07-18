/**
 * @file servo.c
 * @brief Servo driver — LEDC-based hobby servo actuator, 50 Hz.
 *
 * Commands accepted (via MQTT command topic):
 *   {"action": "rotate", "params": {"angle": 90, "hold_ms": 1000}}
 *     Move to `angle`, hold for `hold_ms`, then return to `home_angle`.
 *     Non-blocking — the return move runs on a FreeRTOS one-shot timer so
 *     the MQTT/command task is never blocked waiting for the hold to elapse.
 *
 * Capabilities:
 *   {instance}.angle   writable float, degrees, clamped to [0, max_angle]
 *
 * Idle detach: to avoid servo hum/jitter while holding position, the LEDC
 * signal is stopped ~500ms after the last commanded move (`idle_detach`,
 * default true). The signal is re-attached automatically on the next move.
 */

#include "servo.h"
#include "jettyd_driver.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "drv_servo";

static servo_config_t s_cfg;
static jettyd_driver_t s_driver;
static float s_current_angle = 0.0f;
static bool s_attached = false;
static TimerHandle_t s_idle_timer = NULL;   /* fires ~500ms after last move -> detach */
static TimerHandle_t s_hold_timer = NULL;   /* one-shot for rotate command's hold+return */

#define SERVO_LEDC_TIMER     LEDC_TIMER_0
#define SERVO_LEDC_MODE      LEDC_LOW_SPEED_MODE
#define SERVO_LEDC_DUTY_RES  LEDC_TIMER_13_BIT
#define SERVO_LEDC_MAX_DUTY  ((1 << 13) - 1)
#define SERVO_FREQ_HZ        50
#define SERVO_PERIOD_US      (1000000 / SERVO_FREQ_HZ)
#define SERVO_IDLE_DETACH_MS 500

/* ── Pure math helpers (unit tested directly) ─────────────────────────────── */

static float clamp_angle(float angle)
{
    if (angle < 0.0f) return 0.0f;
    if (angle > s_cfg.max_angle) return s_cfg.max_angle;
    return angle;
}

static uint32_t angle_to_pulse_us(float angle)
{
    angle = clamp_angle(angle);
    if (s_cfg.max_angle <= 0.0f) return s_cfg.min_pulse_us;
    float span = (float)(s_cfg.max_pulse_us - s_cfg.min_pulse_us);
    float frac = angle / s_cfg.max_angle;
    return s_cfg.min_pulse_us + (uint32_t)(frac * span);
}

static uint32_t pulse_us_to_duty(uint32_t pulse_us)
{
    return (uint32_t)(((uint64_t)pulse_us * SERVO_LEDC_MAX_DUTY) / SERVO_PERIOD_US);
}

/* ── LEDC attach/detach ────────────────────────────────────────────────────── */

static void ledc_attach(void)
{
    ledc_channel_config_t ch_cfg = {
        .speed_mode = SERVO_LEDC_MODE,
        .channel = s_cfg.ledc_channel,
        .timer_sel = SERVO_LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = s_cfg.pin,
        .duty = 0,
        .hpoint = 0,
    };
    esp_err_t err = ledc_channel_config(&ch_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LEDC channel config failed: %s", esp_err_to_name(err));
        return;
    }
    s_attached = true;
}

static void ledc_detach_callback(TimerHandle_t timer)
{
    (void)timer;
    ledc_stop(SERVO_LEDC_MODE, s_cfg.ledc_channel, 0);
    s_attached = false;
    ESP_LOGI(TAG, "Servo idle-detached (pin %d)", s_cfg.pin);
}

/* ── Motion ────────────────────────────────────────────────────────────────── */

static void move_to(float angle)
{
    angle = clamp_angle(angle);
    s_current_angle = angle;

    if (s_cfg.idle_detach && !s_attached) {
        ledc_attach();
    }

    uint32_t pulse_us = angle_to_pulse_us(angle);
    uint32_t duty = pulse_us_to_duty(pulse_us);
    ledc_set_duty(SERVO_LEDC_MODE, s_cfg.ledc_channel, duty);
    ledc_update_duty(SERVO_LEDC_MODE, s_cfg.ledc_channel);

    ESP_LOGI(TAG, "Servo -> %.1f deg (pulse %lu us, pin %d)",
             angle, (unsigned long)pulse_us, s_cfg.pin);

    if (s_cfg.idle_detach && s_idle_timer) {
        xTimerChangePeriod(s_idle_timer, pdMS_TO_TICKS(SERVO_IDLE_DETACH_MS), 0);
        xTimerStart(s_idle_timer, 0);
    }
}

static void hold_return_callback(TimerHandle_t timer)
{
    (void)timer;
    move_to(s_cfg.home_angle);
}

static esp_err_t servo_rotate(float angle, uint32_t hold_ms)
{
    move_to(angle);

    if (s_hold_timer) {
        xTimerChangePeriod(s_hold_timer, pdMS_TO_TICKS(hold_ms), 0);
        xTimerStart(s_hold_timer, 0);
    }
    return ESP_OK;
}

/* ── Driver ops ──────────────────────────────────────────────────────────── */

static esp_err_t servo_init(const void *config)
{
    const servo_config_t *c = (const servo_config_t *)config;
    s_cfg = *c;

    if (s_cfg.min_pulse_us == 0) s_cfg.min_pulse_us = 500;
    if (s_cfg.max_pulse_us == 0) s_cfg.max_pulse_us = 2500;
    if (s_cfg.max_angle <= 0.0f) s_cfg.max_angle = 180.0f;

    ledc_timer_config_t timer_cfg = {
        .speed_mode = SERVO_LEDC_MODE,
        .timer_num = SERVO_LEDC_TIMER,
        .duty_resolution = SERVO_LEDC_DUTY_RES,
        .freq_hz = SERVO_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LEDC timer config failed: %s", esp_err_to_name(err));
        return err;
    }

    ledc_attach();

    s_idle_timer = xTimerCreate("servo_idle", pdMS_TO_TICKS(SERVO_IDLE_DETACH_MS),
                                 pdFALSE, NULL, ledc_detach_callback);
    s_hold_timer = xTimerCreate("servo_hold", pdMS_TO_TICKS(1000),
                                 pdFALSE, NULL, hold_return_callback);

    move_to(s_cfg.home_angle);

    ESP_LOGI(TAG, "Servo init: pin=%d, ch=%d, min_pulse=%lu us, max_pulse=%lu us, "
             "max_angle=%.1f, home=%.1f, idle_detach=%d",
             s_cfg.pin, s_cfg.ledc_channel,
             (unsigned long)s_cfg.min_pulse_us, (unsigned long)s_cfg.max_pulse_us,
             s_cfg.max_angle, s_cfg.home_angle, s_cfg.idle_detach);
    return ESP_OK;
}

static esp_err_t servo_deinit(void)
{
    if (s_idle_timer) {
        xTimerStop(s_idle_timer, 0);
        xTimerDelete(s_idle_timer, 0);
        s_idle_timer = NULL;
    }
    if (s_hold_timer) {
        xTimerStop(s_hold_timer, 0);
        xTimerDelete(s_hold_timer, 0);
        s_hold_timer = NULL;
    }
    ledc_stop(SERVO_LEDC_MODE, s_cfg.ledc_channel, 0);
    s_attached = false;
    return ESP_OK;
}

static jettyd_value_t servo_read(const char *capability)
{
    jettyd_value_t val = { .type = JETTYD_VAL_FLOAT, .float_val = s_current_angle, .valid = true };
    (void)capability;
    return val;
}

static esp_err_t servo_write(const char *capability, jettyd_value_t value)
{
    if (strcmp(capability, "angle") != 0) return ESP_ERR_NOT_SUPPORTED;

    float angle;
    if (value.type == JETTYD_VAL_FLOAT)      angle = value.float_val;
    else if (value.type == JETTYD_VAL_INT)   angle = (float)value.int_val;
    else return ESP_ERR_INVALID_ARG;

    move_to(angle);
    return ESP_OK;
}

static esp_err_t servo_command(const char *action, const char *params_json)
{
    if (strcmp(action, "rotate") == 0) {
        float angle = 90.0f;
        uint32_t hold_ms = 1000;

        if (params_json) {
            const char *p = strstr(params_json, "\"angle\":");
            if (p) angle = strtof(p + 8, NULL);
            p = strstr(params_json, "\"hold_ms\":");
            if (p) hold_ms = (uint32_t)atoi(p + 10);
        }

        return servo_rotate(angle, hold_ms);
    }
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t servo_self_test(void)
{
    float original = s_current_angle;
    move_to(clamp_angle(original + 10.0f));
    vTaskDelay(pdMS_TO_TICKS(50));
    move_to(original);
    return ESP_OK;
}

static esp_err_t servo_mcp_rotate(const char *params_json, char *out, size_t out_len)
{
    esp_err_t err = servo_command("rotate", params_json);
    if (err == ESP_OK) snprintf(out, out_len, "{\"angle\":%.1f}", s_current_angle);
    return err;
}

void servo_register(const char *instance, const void *config)
{
    memset(&s_driver, 0, sizeof(s_driver));
    strlcpy(s_driver.instance, instance, sizeof(s_driver.instance));
    strlcpy(s_driver.driver_name, "servo", sizeof(s_driver.driver_name));

    s_driver.capability_count = 1;
    strlcpy(s_driver.capabilities[0].name, "angle", sizeof(s_driver.capabilities[0].name));
    s_driver.capabilities[0].type = JETTYD_CAP_WRITABLE;
    s_driver.capabilities[0].value_type = JETTYD_VAL_FLOAT;
    s_driver.capabilities[0].min_value = 0.0f;
    s_driver.capabilities[0].max_value = ((const servo_config_t *)config)->max_angle > 0.0f
                                              ? ((const servo_config_t *)config)->max_angle
                                              : 180.0f;
    strlcpy(s_driver.capabilities[0].unit, "deg", sizeof(s_driver.capabilities[0].unit));

    s_driver.init = servo_init;
    s_driver.deinit = servo_deinit;
    s_driver.read = servo_read;
    s_driver.write = servo_write;
    s_driver.command = servo_command;
    s_driver.self_test = servo_self_test;

    static const char *schema_rotate =
        "{\"type\":\"object\",\"properties\":{\"angle\":{\"type\":\"number\"},\"hold_ms\":{\"type\":\"number\"}}}";

    strlcpy(s_driver.mcp_tools[0].name, "servo_rotate", sizeof(s_driver.mcp_tools[0].name));
    s_driver.mcp_tools[0].description = "Rotate the servo to an angle, hold, then return home";
    s_driver.mcp_tools[0].input_schema_json = schema_rotate;
    s_driver.mcp_tools[0].handler = servo_mcp_rotate;
    s_driver.mcp_tool_count = 1;

    JETTYD_REGISTER_DRIVER(&s_driver);

    servo_init(config);
}
