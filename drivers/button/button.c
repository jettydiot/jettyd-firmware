/**
 * @file button.c
 * @brief Button/switch driver — GPIO input with debounce, press event publishing.
 *
 * Events published via jettyd telemetry:
 *
 *   instance.press        bool   — true on any press (short or long)
 *   instance.press_count  float  — total presses since boot
 *   instance.long_press   bool   — true when long press threshold exceeded
 *   instance.double_press bool   — true when two presses within double_press_ms
 *
 * Config:
 *   pin            GPIO number
 *   active_low     true = button pulls pin to GND (internal pull-up enabled)
 *   debounce_ms    edge debounce window (default 50ms)
 *   long_press_ms  hold duration for long press event (default 500ms, 0=disabled)
 *   double_press_ms max gap between two presses for double press (default 300ms, 0=disabled)
 *
 * Wiring: GPIO pin → button → GND (active_low=true, internal pull-up)
 *         3.3V → button → GPIO pin (active_low=false, internal pull-down)
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

/* ── State ───────────────────────────────────────────────────────────────── */

static button_config_t s_cfg;
static jettyd_driver_t s_driver;
static char s_instance[JETTYD_MAX_INSTANCE_NAME];

/* ISR-updated state (volatile) */
static volatile bool     s_pressed       = false;
static volatile uint32_t s_press_count   = 0;
static volatile int64_t  s_press_time_us = 0;   /* when current press started */
static volatile int64_t  s_last_edge_us  = 0;

/*
 * Task-side state machine
 *
 *   IDLE ──press──► ARMED ──hold≥long──► LONG_FIRED
 *                      │
 *                   release
 *                      │
 *                   PENDING ──timeout──► emit SHORT, → IDLE
 *                      │
 *                   press again within double_ms
 *                      │
 *                   emit DOUBLE, wait for release → IDLE
 */
typedef enum {
    BTN_IDLE,
    BTN_ARMED,       /* pressed, not yet determined */
    BTN_LONG_FIRED,  /* long press already emitted, waiting for release */
    BTN_PENDING,     /* released once, waiting to see if double press follows */
} btn_state_t;

static btn_state_t s_state           = BTN_IDLE;
static uint32_t    s_armed_count     = 0;    /* press_count when we entered ARMED */
static int64_t     s_arm_time_us     = 0;    /* when ARMED was entered */
static int64_t     s_release_time_us = 0;    /* when we entered PENDING */

/* ── ISR ─────────────────────────────────────────────────────────────────── */

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
        s_press_time_us = now;
    }
    s_pressed = pressed;
}

/* ── Event task ──────────────────────────────────────────────────────────── */

static void button_event_task(void *arg)
{
    (void)arg;

    char m_press[48], m_count[48], m_long[48], m_double[48];
    snprintf(m_press,  sizeof(m_press),  "%s.press",        s_instance);
    snprintf(m_count,  sizeof(m_count),  "%s.press_count",  s_instance);
    snprintf(m_long,   sizeof(m_long),   "%s.long_press",   s_instance);
    snprintf(m_double, sizeof(m_double), "%s.double_press", s_instance);

    int64_t long_us   = (int64_t)(s_cfg.long_press_ms   ? s_cfg.long_press_ms   : 500) * 1000;
    int64_t double_us = (int64_t)(s_cfg.double_press_ms ? s_cfg.double_press_ms : 300) * 1000;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(20)); /* 20ms tick for responsive detection */

        int64_t  now     = esp_timer_get_time();
        bool     pressed = s_pressed;
        uint32_t count   = s_press_count;

        switch (s_state) {

        case BTN_IDLE:
            if (pressed) {
                s_state       = BTN_ARMED;
                s_armed_count = count;
                s_arm_time_us = now;
            }
            break;

        case BTN_ARMED:
            if (!pressed) {
                /* Released — could still be a double press, wait */
                s_state          = BTN_PENDING;
                s_release_time_us = now;
            } else if (s_cfg.long_press_ms != 0
                       && (now - s_arm_time_us) >= long_us) {
                /* Held long enough — fire long press immediately */
                int64_t held_ms = (now - s_arm_time_us) / 1000;
                ESP_LOGI(TAG, "Button '%s' long press (held %" PRId64 "ms)",
                         s_instance, held_ms);
                const char *metrics[] = { m_long, m_press, m_count };
                jettyd_telemetry_publish(metrics, 3);
                s_state = BTN_LONG_FIRED;
            }
            break;

        case BTN_LONG_FIRED:
            /* Wait for release before accepting next press */
            if (!pressed) {
                s_state = BTN_IDLE;
            }
            break;

        case BTN_PENDING:
            if (pressed) {
                /* Second press — is it within the double-press window? */
                if (s_cfg.double_press_ms != 0
                    && (now - s_release_time_us) < double_us) {
                    /* Fire double press immediately on second press-down */
                    ESP_LOGI(TAG, "Button '%s' double press (total: %" PRIu32 ")",
                             s_instance, count);
                    const char *metrics[] = { m_double, m_press, m_count };
                    jettyd_telemetry_publish(metrics, 3);
                    s_state = BTN_LONG_FIRED; /* reuse "wait for release" state */
                } else {
                    /* Outside double window — treat as new single press */
                    ESP_LOGI(TAG, "Button '%s' press (total: %" PRIu32 ")",
                             s_instance, s_armed_count);
                    const char *metrics[] = { m_press, m_count };
                    jettyd_telemetry_publish(metrics, 2);
                    /* Start tracking this new press */
                    s_state       = BTN_ARMED;
                    s_armed_count = count;
                    s_arm_time_us = now;
                }
            } else if ((now - s_release_time_us) >= double_us) {
                /* Double-press window expired — commit as single press */
                ESP_LOGI(TAG, "Button '%s' press (total: %" PRIu32 ")",
                         s_instance, s_armed_count);
                const char *metrics[] = { m_press, m_count };
                jettyd_telemetry_publish(metrics, 2);
                s_state = BTN_IDLE;
            }
            break;
        }
    }
}

/* ── Driver ops ──────────────────────────────────────────────────────────── */

static esp_err_t button_init(const void *config)
{
    const button_config_t *c = (const button_config_t *)config;
    s_cfg = *c;
    if (s_cfg.debounce_ms == 0) s_cfg.debounce_ms = 50;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << s_cfg.pin),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = s_cfg.active_low ? GPIO_PULLUP_ENABLE  : GPIO_PULLUP_DISABLE,
        .pull_down_en = s_cfg.active_low ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) return err;

    esp_err_t isr_err = gpio_install_isr_service(0);
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) return isr_err;
    gpio_isr_handler_add(s_cfg.pin, gpio_isr_handler, NULL);

    /* 4096 bytes — needed for float formatting in jettyd_telemetry_publish */
    xTaskCreate(button_event_task, "btn_event", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Button init: GPIO %d, active_%s, debounce %"PRIu32"ms, "
             "long_press %"PRIu32"ms, double_press %"PRIu32"ms",
             s_cfg.pin,
             s_cfg.active_low ? "low" : "high",
             s_cfg.debounce_ms,
             s_cfg.long_press_ms  ? s_cfg.long_press_ms  : 500U,
             s_cfg.double_press_ms ? s_cfg.double_press_ms : 300U);
    return ESP_OK;
}

static jettyd_value_t button_read(const char *metric)
{
    jettyd_value_t v = { .valid = true };
    if (strcmp(metric, "press") == 0) {
        v.type     = JETTYD_VAL_BOOL;
        v.bool_val = s_pressed;
    } else if (strcmp(metric, "press_count") == 0) {
        v.type      = JETTYD_VAL_FLOAT;
        v.float_val = (float)s_press_count;
    } else if (strcmp(metric, "long_press") == 0) {
        v.type     = JETTYD_VAL_BOOL;
        v.bool_val = (s_state == BTN_LONG_FIRED);
    } else if (strcmp(metric, "double_press") == 0) {
        v.type     = JETTYD_VAL_BOOL;
        v.bool_val = false; /* stateless — events are momentary */
    } else {
        v.valid = false;
    }
    return v;
}

/* ── Registration ────────────────────────────────────────────────────────── */

void button_register(const char *instance, const void *config)
{
    memset(&s_driver, 0, sizeof(s_driver));
    strlcpy(s_driver.driver_name, "button",   sizeof(s_driver.driver_name));
    strlcpy(s_driver.instance,    instance,   sizeof(s_driver.instance));
    strlcpy(s_instance,           instance,   sizeof(s_instance));

    strlcpy(s_driver.capabilities[0].name, "press",        sizeof(s_driver.capabilities[0].name));
    s_driver.capabilities[0].type       = JETTYD_CAP_READABLE;
    s_driver.capabilities[0].value_type = JETTYD_VAL_BOOL;

    strlcpy(s_driver.capabilities[1].name, "press_count",  sizeof(s_driver.capabilities[1].name));
    s_driver.capabilities[1].type       = JETTYD_CAP_READABLE;
    s_driver.capabilities[1].value_type = JETTYD_VAL_FLOAT;

    strlcpy(s_driver.capabilities[2].name, "long_press",   sizeof(s_driver.capabilities[2].name));
    s_driver.capabilities[2].type       = JETTYD_CAP_READABLE;
    s_driver.capabilities[2].value_type = JETTYD_VAL_BOOL;

    strlcpy(s_driver.capabilities[3].name, "double_press", sizeof(s_driver.capabilities[3].name));
    s_driver.capabilities[3].type       = JETTYD_CAP_READABLE;
    s_driver.capabilities[3].value_type = JETTYD_VAL_BOOL;

    s_driver.capability_count = 4;
    s_driver.init = button_init;
    s_driver.read = button_read;

    jettyd_driver_registry_add(&s_driver);
    button_init(config);
}
