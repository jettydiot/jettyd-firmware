/**
 * @file camera.h
 * @brief Camera driver (ESP32-S3 + OV2640) for the Jettyd firmware SDK.
 *
 * Captures JPEG images on trigger (interval timer or MCP command) and
 * uploads via the media/request → media/grant → media/complete grant flow.
 * Requires PSRAM; compiles to a no-op stub on non-camera targets (esp32c3).
 */

#ifndef JETTYD_DRIVER_CAMERA_H
#define JETTYD_DRIVER_CAMERA_H

#include "jettyd_driver.h"
#include <stdint.h>

/* ------------------------------------------------------------------
 * Enums
 * ------------------------------------------------------------------ */

typedef enum {
    CAMERA_SENSOR_OV2640  = 0,
    CAMERA_SENSOR_UNKNOWN = -1,
} camera_sensor_t;

typedef enum {
    CAMERA_FRAME_QVGA = 0, /**< 320x240 */
    CAMERA_FRAME_VGA,      /**< 640x480 */
    CAMERA_FRAME_SVGA,     /**< 800x600 (default) */
    CAMERA_FRAME_XGA,      /**< 1024x768 */
    CAMERA_FRAME_UXGA,     /**< 1600x1200 */
} camera_frame_size_t;

/* ------------------------------------------------------------------
 * Config struct (populated by codegen from device.yaml)
 *
 * All DVP pin assignments for the selected sensor are encoded in a
 * named preset constant inside camera.c — no hardcoded pins in logic.
 * ------------------------------------------------------------------ */

typedef struct {
    camera_sensor_t     sensor;               /**< ov2640 only (v1) */
    camera_frame_size_t frame_size;           /**< JPEG output resolution */
    uint8_t             jpeg_quality;         /**< 0–63; lower = better quality */
    uint32_t            capture_interval_sec; /**< Periodic capture; 0 = disabled */
    uint32_t            grant_timeout_sec;    /**< media/grant wait timeout (default 30) */
} camera_config_t;

/* ------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------ */

/**
 * @brief Validate and normalise a camera_config_t.
 *
 * Rejects unsupported sensors (ESP_ERR_INVALID_ARG).
 * Silently clamps jpeg_quality > 63 to 63.
 * Fills grant_timeout_sec default (30 s) when 0.
 *
 * @param cfg  Config to validate (modified in-place for clamping).
 * @return ESP_OK or ESP_ERR_INVALID_ARG.
 */
esp_err_t camera_config_validate(camera_config_t *cfg);

/**
 * @brief Register a camera driver instance.
 *
 * On camera-capable targets (ESP32-S3): initialises the OV2640,
 * subscribes to media/grant, starts the interval timer (if configured),
 * and registers the camera_capture MCP tool.
 *
 * On non-camera targets (esp32c3, etc.): logs a message and returns
 * cleanly without registering anything.
 *
 * @param instance  Instance name (e.g., "cam").
 * @param config    Pointer to camera_config_t.
 */
void camera_register(const char *instance, const void *config);

#endif /* JETTYD_DRIVER_CAMERA_H */
