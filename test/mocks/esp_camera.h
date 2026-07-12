/**
 * @file esp_camera.h
 * @brief Host-test mock for the espressif/esp32-camera component.
 *
 * Mirrors the real API surface the driver uses: the HW-init struct is named
 * camera_config_t (as in the real component), and the frame-size constants use
 * the real, NON-CONTIGUOUS framesize_t values from the component's sensor.h.
 * The non-contiguous values are deliberate: if the driver ever casts its own
 * camera_frame_size_t enum straight onto framesize_t (instead of mapping it),
 * the wrong frame size is selected and the host tests catch the regression.
 *
 * The driver's user-facing config struct is camera_driver_config_t (declared in
 * camera.h) — a distinct type, so there is no name clash when both headers are
 * included in camera.c.
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

/* pixformat_t — only the values used by the driver preset. */
typedef enum {
    PIXFORMAT_RGB565    = 0,
    PIXFORMAT_YUV422    = 1,
    PIXFORMAT_GRAYSCALE = 3,
    PIXFORMAT_JPEG      = 4,
} pixformat_t;

/* framesize_t — real esp32-camera values (sensor.h). NON-CONTIGUOUS with the
 * driver's camera_frame_size_t enum (QVGA=0..UXGA=4) on purpose. */
typedef enum {
    FRAMESIZE_96X96   = 0,
    FRAMESIZE_QQVGA   = 1,
    FRAMESIZE_QCIF    = 2,
    FRAMESIZE_HQVGA   = 3,
    FRAMESIZE_240X240 = 4,
    FRAMESIZE_QVGA    = 5,
    FRAMESIZE_CIF     = 6,
    FRAMESIZE_HVGA    = 7,
    FRAMESIZE_VGA     = 8,
    FRAMESIZE_SVGA    = 9,
    FRAMESIZE_XGA     = 10,
    FRAMESIZE_HD      = 11,
    FRAMESIZE_SXGA    = 12,
    FRAMESIZE_UXGA    = 13,
} framesize_t;

typedef enum {
    CAMERA_FB_IN_PSRAM = 0,
    CAMERA_FB_IN_DRAM  = 1,
} camera_fb_location_t;

typedef enum {
    CAMERA_GRAB_WHEN_EMPTY = 0,
    CAMERA_GRAB_LATEST     = 1,
} camera_grab_mode_t;

/* Real esp32-camera HW-init struct — this IS named camera_config_t in the
 * component. It does NOT clash with the driver's camera_driver_config_t. */
typedef struct {
    int pin_pwdn;
    int pin_reset;
    int pin_xclk;
    int pin_sccb_sda;
    int pin_sccb_scl;
    int pin_d7, pin_d6, pin_d5, pin_d4, pin_d3, pin_d2, pin_d1, pin_d0;
    int pin_vsync;
    int pin_href;
    int pin_pclk;
    int xclk_freq_hz;
    ledc_timer_t         ledc_timer;
    ledc_channel_t       ledc_channel;
    pixformat_t          pixel_format;
    framesize_t          frame_size;
    int                  jpeg_quality;
    int                  fb_count;
    camera_fb_location_t fb_location;
    camera_grab_mode_t   grab_mode;
} camera_config_t;

/* ------------------------------------------------------------------
 * Test-control globals (defined in the test file)
 * ------------------------------------------------------------------ */

extern camera_fb_t *g_test_fb;             /**< Framebuffer returned by fb_get */
extern bool         g_camera_init_ok;      /**< Whether esp_camera_init succeeds */
extern int          g_fb_return_count;     /**< How many times fb_return was called */
extern int          g_camera_init_frame_size;  /**< framesize_t seen by esp_camera_init */
extern int          g_camera_init_fb_location; /**< fb_location seen by esp_camera_init */

/* ------------------------------------------------------------------
 * Inline stubs
 * ------------------------------------------------------------------ */

static inline esp_err_t esp_camera_init(const camera_config_t *c)
{
    if (c) {
        g_camera_init_frame_size  = (int)c->frame_size;
        g_camera_init_fb_location = (int)c->fb_location;
    }
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
