/**
 * @file esp_idf_stubs.h
 * @brief Minimal ESP-IDF stubs for host-side unit testing.
 *
 * Include this before any jettyd SDK headers when compiling tests
 * on the host (macOS/Linux). Provides just enough of the ESP-IDF
 * surface area to compile and run logic tests.
 */

#ifndef ESP_IDF_STUBS_H
#define ESP_IDF_STUBS_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <inttypes.h>
#include <time.h>

/* ── esp_err.h ──────────────────────────────────────────────────────────── */

typedef int esp_err_t;
#define ESP_OK                0
#define ESP_FAIL              (-1)
#define ESP_ERR_INVALID_ARG   0x102
#define ESP_ERR_NO_MEM        0x101
#define ESP_ERR_NOT_FOUND     0x105
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT       0x107

static inline const char *esp_err_to_name(esp_err_t err)
{
    switch (err) {
        case ESP_OK:                return "ESP_OK";
        case ESP_FAIL:              return "ESP_FAIL";
        case ESP_ERR_INVALID_ARG:   return "ESP_ERR_INVALID_ARG";
        case ESP_ERR_NO_MEM:        return "ESP_ERR_NO_MEM";
        case ESP_ERR_NOT_FOUND:     return "ESP_ERR_NOT_FOUND";
        case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
        case ESP_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
        default:                    return "ESP_ERR_UNKNOWN";
    }
}

#define ESP_ERROR_CHECK(x) do { \
    esp_err_t _err = (x); \
    if (_err != ESP_OK) { \
        fprintf(stderr, "ESP_ERROR_CHECK failed: %s at %s:%d\n", \
                esp_err_to_name(_err), __FILE__, __LINE__); \
        abort(); \
    } \
} while(0)

/* ── esp_log.h ──────────────────────────────────────────────────────────── */

#define ESP_LOGI(tag, fmt, ...) printf("[I][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("[W][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "[E][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) /* debug suppressed in tests */
#define ESP_LOGV(tag, fmt, ...) /* verbose suppressed in tests */

/* ── esp_timer.h ────────────────────────────────────────────────────────── */

static inline int64_t esp_timer_get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + (int64_t)ts.tv_nsec / 1000LL;
}

/* ── string extras ──────────────────────────────────────────────────────── */

#ifndef strlcpy
static inline size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);
    if (size > 0) {
        size_t copy = len < size - 1 ? len : size - 1;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return len;
}
#endif

/* ── FreeRTOS stubs (no-op for host tests) ──────────────────────────────── */

#define pdMS_TO_TICKS(x)    (x)
#define pdTRUE              1
#define pdFALSE             0
#define pdPASS              pdTRUE

typedef void *TaskHandle_t;
typedef void *TimerHandle_t;
typedef uint32_t TickType_t;
typedef unsigned int UBaseType_t;
typedef int BaseType_t;

#define xTaskCreate(fn, name, stack, param, prio, handle) (pdPASS)
#define vTaskDelay(ticks)       ((void)0)
#define vTaskDelete(handle)     ((void)0)
#define xTimerCreate(n,p,r,i,cb) (NULL)
#define xTimerStart(t, w)       (pdPASS)
#define xTimerStop(t, w)        (pdPASS)
#define xTimerChangePeriod(t,p,w) (pdPASS)

/* ── GPIO stubs ─────────────────────────────────────────────────────────── */

typedef int gpio_num_t;

typedef enum {
    GPIO_MODE_INPUT,
    GPIO_MODE_OUTPUT,
    GPIO_MODE_INPUT_OUTPUT,
} gpio_mode_t;

typedef enum {
    GPIO_PULLUP_DISABLE = 0,
    GPIO_PULLUP_ENABLE  = 1,
} gpio_pullup_t;

typedef enum {
    GPIO_PULLDOWN_DISABLE = 0,
    GPIO_PULLDOWN_ENABLE  = 1,
} gpio_pulldown_t;

typedef enum {
    GPIO_INTR_DISABLE  = 0,
    GPIO_INTR_ANYEDGE  = 1,
    GPIO_INTR_POSEDGE  = 2,
    GPIO_INTR_NEGEDGE  = 3,
} gpio_int_type_t;

typedef struct {
    uint64_t        pin_bit_mask;
    gpio_mode_t     mode;
    gpio_pullup_t   pull_up_en;
    gpio_pulldown_t pull_down_en;
    gpio_int_type_t intr_type;
} gpio_config_t;

/* Simulated GPIO state — tests can set these directly */
static int g_gpio_levels[48] = {0};

static inline esp_err_t gpio_config(const gpio_config_t *cfg) { (void)cfg; return ESP_OK; }
static inline esp_err_t gpio_set_level(gpio_num_t gpio, uint32_t level)
{
    if (gpio >= 0 && gpio < 48) g_gpio_levels[gpio] = (int)level;
    return ESP_OK;
}
static inline int gpio_get_level(gpio_num_t gpio)
{
    if (gpio >= 0 && gpio < 48) return g_gpio_levels[gpio];
    return 0;
}
static inline esp_err_t gpio_install_isr_service(int flags) { (void)flags; return ESP_OK; }
typedef void (*gpio_isr_t)(void *);
static inline esp_err_t gpio_isr_handler_add(gpio_num_t gpio, gpio_isr_t isr, void *arg)
{
    (void)gpio; (void)isr; (void)arg;
    return ESP_OK;
}

/* ── driver/ledc.h stubs (for pwm_output) ──────────────────────────────── */

typedef int ledc_channel_t;
typedef int ledc_mode_t;
typedef int ledc_timer_t;
typedef int ledc_timer_bit_t;
#define LEDC_HIGH_SPEED_MODE 0
#define LEDC_TIMER_13_BIT    13
#define LEDC_TIMER_0         0

typedef struct { int x; } ledc_timer_config_t;
typedef struct { int x; } ledc_channel_config_t;
static inline esp_err_t ledc_timer_config(const ledc_timer_config_t *c)   { (void)c; return ESP_OK; }
static inline esp_err_t ledc_channel_config(const ledc_channel_config_t *c) { (void)c; return ESP_OK; }
static inline esp_err_t ledc_set_duty(ledc_mode_t m, ledc_channel_t c, uint32_t d) { (void)m; (void)c; (void)d; return ESP_OK; }
static inline esp_err_t ledc_update_duty(ledc_mode_t m, ledc_channel_t c)  { (void)m; (void)c; return ESP_OK; }
static inline esp_err_t ledc_stop(ledc_mode_t m, ledc_channel_t c, uint32_t l) { (void)m; (void)c; (void)l; return ESP_OK; }

/* ── IRAM_ATTR (no-op on host) ──────────────────────────────────────────── */
#define IRAM_ATTR

#endif /* ESP_IDF_STUBS_H */
