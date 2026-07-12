/**
 * @file esp_camera.h
 * @brief Host-test mock for the espressif/esp32-camera component.
 *
 * Provides camera_fb_t, esp_camera_init/deinit/fb_get/fb_return stubs.
 * Named esp_cam_hw_config_t for the HW config to avoid collision with
 * the driver's own camera_config_t (user-facing config struct).
 * Test files define the extern control variables below.
 */

#pragma once
#include "esp_idf_stubs.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------
 * Types matching the real esp32-camera API
 * ------------------------------------------------------------------ */

typedef struct {
    uint8_t *buf;
    size_t   len;
    size_t   width;
    size_t   height;
    int      format;
} camera_fb_t;

typedef struct { int model; } sensor_t;

/* HW config struct (real esp32-camera calls this camera_config_t; we
   rename here to avoid collision with the driver's user-facing type). */
typedef struct {
    int pin_d0, pin_d1, pin_d2, pin_d3, pin_d4, pin_d5, pin_d6, pin_d7;
    int pin_xclk, pin_pclk, pin_vsync, pin_href;
    int pin_sscb_sda, pin_sscb_scl;
    int pin_pwdn, pin_reset;
    int xclk_freq_hz;
    int pixel_format;
    int frame_size;
    int jpeg_quality;
    int fb_count;
    int grab_mode;
} esp_cam_hw_config_t;

/* PIXFORMAT_JPEG = 4 in real esp32-camera */
#define PIXFORMAT_JPEG      4
/* CAMERA_GRAB_WHEN_EMPTY = 0 */
#define CAMERA_GRAB_WHEN_EMPTY 0

/* ------------------------------------------------------------------
 * Test-control globals (defined in the test file)
 * ------------------------------------------------------------------ */

extern camera_fb_t *g_test_fb;       /**< Framebuffer returned by fb_get */
extern bool         g_camera_init_ok; /**< Whether esp_camera_init succeeds */
extern int          g_fb_return_count;/**< How many times fb_return was called */

/* ------------------------------------------------------------------
 * Inline stubs
 * ------------------------------------------------------------------ */

static inline esp_err_t esp_camera_init(const esp_cam_hw_config_t *c)
{
    (void)c;
    return g_camera_init_ok ? ESP_OK : ESP_FAIL;
}

static inline esp_err_t esp_camera_deinit(void) { return ESP_OK; }

static inline camera_fb_t *esp_camera_fb_get(void) { return g_test_fb; }

static inline void esp_camera_fb_return(camera_fb_t *fb)
{
    (void)fb;
    g_fb_return_count++;
}

static inline sensor_t *esp_camera_sensor_get(void) { return NULL; }
