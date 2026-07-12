/**
 * @file camera.c
 * @brief Camera driver (ESP32-S3 + OV2640) — capture and upload via media grant flow.
 *
 * Capture triggers:
 *   1. Interval timer when capture_interval_sec > 0
 *   2. camera_capture MCP tool (non-blocking, enqueues via trigger function)
 *
 * Upload state machine:
 *   capture → publish media/request → await media/grant (timeout via timer)
 *   → hand off grant to the dedicated upload task → HTTPS PUT framebuffer
 *   (streamed, sha256 incremental) → publish media/complete
 *
 * Threading model — three tasks touch the state machine:
 *   - MQTT event task     : delivers media/grant (camera_on_grant_received)
 *   - Timer daemon        : grant timeout (camera_grant_timeout_cb)
 *   - MCP dispatch task    : camera_capture tool
 *   - Camera upload task   : performs the blocking HTTPS PUT (camera_upload_task)
 * State transitions are serialised with a portMUX critical section. The grant
 * callback NEVER performs the blocking upload itself — it only parses/validates
 * and hands the presigned URL to the upload task via a FreeRTOS queue, so the
 * MQTT client task is never stalled by a multi-second TLS transfer.
 *
 * Quota backoff: media/grant with quota_exceeded + retry_after_sec suppresses
 * further requests until the retry window elapses.
 *
 * PSRAM required: init fails with ESP_ERR_NOT_FOUND when PSRAM is absent.
 *
 * Target guard: all camera/HTTP code is compiled only when the Kconfig symbol
 * CONFIG_JETTYD_CAMERA_SUPPORTED is defined (auto-selected for esp32s3 via
 * Kconfig, or via -DCONFIG_JETTYD_CAMERA_SUPPORTED=1 in host tests). On
 * non-camera targets the file compiles to a no-op stub.
 */

#include "camera.h"
#include "jettyd_driver.h"
#include "jettyd_mqtt.h"
#include "jettyd_shadow.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

static const char *TAG = "drv_camera";

/* ------------------------------------------------------------------
 * Config validation — always compiled (pure, host-testable)
 * ------------------------------------------------------------------ */

esp_err_t camera_config_validate(camera_driver_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    if (cfg->sensor != CAMERA_SENSOR_OV2640) {
        ESP_LOGE(TAG, "Unsupported sensor %d (only ov2640 supported in v1)",
                 (int)cfg->sensor);
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->jpeg_quality > 63) {
        ESP_LOGW(TAG, "jpeg_quality %u > 63, clamped to 63", cfg->jpeg_quality);
        cfg->jpeg_quality = 63;
    }
    if (cfg->grant_timeout_sec == 0) {
        cfg->grant_timeout_sec = 30;
    }
    return ESP_OK;
}

/* ==================================================================
 * Camera-capable implementation (ESP32-S3 / host tests with mock)
 * ================================================================== */

#if defined(CONFIG_JETTYD_CAMERA_SUPPORTED)

#include "esp_camera.h"
#include "esp_psram.h"
#include "esp_http_client.h"
#include "mbedtls/sha256.h"

/* ------------------------------------------------------------------
 * OV2640 DVP pin preset for standard ESP32-S3-CAM board wiring.
 * All DVP assignments live here — never scattered in runtime logic.
 * This is the real esp32-camera `camera_config_t`; frame_size and
 * jpeg_quality are filled per-instance in camera_hw_init().
 * ------------------------------------------------------------------ */

static const camera_config_t s_ov2640_s3cam_preset = {
    .pin_pwdn     = -1,
    .pin_reset    = -1,
    .pin_xclk     = 10,
    .pin_sccb_sda =  4,
    .pin_sccb_scl =  5,
    .pin_d0  = 39, .pin_d1  = 40, .pin_d2 = 41, .pin_d3 = 42,
    .pin_d4  = 43, .pin_d5  = 44, .pin_d6 = 45, .pin_d7 = 48,
    .pin_vsync    =  6,
    .pin_href     =  7,
    .pin_pclk     = 11,
    .xclk_freq_hz = 20000000,
    .ledc_timer   = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,
    .pixel_format = PIXFORMAT_JPEG,
    .frame_size   = FRAMESIZE_SVGA,        /* overridden per instance */
    .jpeg_quality = 12,                    /* overridden per instance */
    .fb_count     = 2,
    .fb_location  = CAMERA_FB_IN_PSRAM,
    .grab_mode    = CAMERA_GRAB_WHEN_EMPTY,
};

/* ------------------------------------------------------------------
 * Driver enum → real esp32-camera framesize_t mapping.
 *
 * The two enumerations are NOT numerically aligned (real framesize_t has
 * many intermediate sizes), so an (int) cast would silently pick the wrong
 * resolution. Map explicitly.
 * ------------------------------------------------------------------ */

static framesize_t camera_map_frame_size(camera_frame_size_t fs)
{
    switch (fs) {
        case CAMERA_FRAME_QVGA: return FRAMESIZE_QVGA;
        case CAMERA_FRAME_VGA:  return FRAMESIZE_VGA;
        case CAMERA_FRAME_SVGA: return FRAMESIZE_SVGA;
        case CAMERA_FRAME_XGA:  return FRAMESIZE_XGA;
        case CAMERA_FRAME_UXGA: return FRAMESIZE_UXGA;
        default:                return FRAMESIZE_SVGA;
    }
}

/* ------------------------------------------------------------------
 * Driver state
 * ------------------------------------------------------------------ */

static camera_driver_config_t s_cfg;
static jettyd_driver_t        s_driver;
static char                   s_instance[JETTYD_MAX_INSTANCE_NAME];

/* Upload state machine.
 *   IDLE       — nothing in flight
 *   AWAIT_GRANT — media/request sent, waiting for media/grant (timer armed)
 *   UPLOADING   — grant accepted, framebuffer owned by the upload task
 * The timeout callback only acts on AWAIT_GRANT; once we transition to
 * UPLOADING (under the mux) a late timeout callback is a no-op, so it can
 * never release a framebuffer that the upload task is streaming. */
typedef enum {
    CAM_STATE_IDLE = 0,
    CAM_STATE_AWAIT_GRANT,
    CAM_STATE_UPLOADING,
} cam_upload_state_t;

/* Work item handed to the dedicated upload task. */
typedef struct {
    char         url[512];
    char         request_id[17];
    camera_fb_t *fb;
    size_t       fb_len;
} cam_upload_job_t;

static portMUX_TYPE       s_state_mux        = portMUX_INITIALIZER_UNLOCKED;
static cam_upload_state_t s_upload_state     = CAM_STATE_IDLE;
static camera_fb_t       *s_fb               = NULL;
static int64_t            s_grant_deadline   = 0; /* monotonic µs */
static int64_t            s_quota_retry_at   = 0; /* monotonic µs; 0 = no backoff */
static uint32_t           s_upload_errors    = 0;
static uint32_t           s_req_counter      = 0;
static char               s_request_id[17]   = {0};

static TimerHandle_t s_interval_timer      = NULL;
static TimerHandle_t s_grant_timeout_timer = NULL;
static QueueHandle_t s_upload_queue        = NULL;
static TaskHandle_t  s_upload_task         = NULL;

/* ------------------------------------------------------------------
 * Shadow error reporting
 * ------------------------------------------------------------------ */

static void camera_push_error_to_shadow(void)
{
    jettyd_value_t v = {
        .type    = JETTYD_VAL_INT,
        .int_val = (int32_t)s_upload_errors,
        .valid   = true,
    };
    char key[JETTYD_MAX_INSTANCE_NAME + 24];
    snprintf(key, sizeof(key), "%s.upload_errors", s_instance);
    jettyd_shadow_update(key, v);
}

/* ------------------------------------------------------------------
 * Release framebuffer and record a failure.
 *
 * Safe to call from any of the state-machine tasks: the state transition
 * and framebuffer ownership hand-off happen inside the critical section,
 * and the (non-reentrant) fb_return runs afterwards on the local pointer.
 * ------------------------------------------------------------------ */

static void camera_release_fb_and_fail(const char *reason)
{
    ESP_LOGW(TAG, "Camera upload failed: %s", reason);

    portENTER_CRITICAL(&s_state_mux);
    camera_fb_t *fb = s_fb;
    s_fb           = NULL;
    s_upload_state = CAM_STATE_IDLE;
    portEXIT_CRITICAL(&s_state_mux);

    if (fb) esp_camera_fb_return(fb);
    s_upload_errors++;
    camera_push_error_to_shadow();
}

/* ------------------------------------------------------------------
 * Streamed HTTPS PUT with incremental sha256
 *
 * Reads directly from the supplied framebuffer — no full-image heap copy.
 * Fills sha_out[32] with raw SHA-256 bytes on success.
 * ------------------------------------------------------------------ */

static esp_err_t camera_do_upload(const char *url, camera_fb_t *fb, uint8_t *sha_out)
{
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0 /* sha256, not sha224 */);

    esp_http_client_config_t http_cfg = {
        .url    = url,
        .method = HTTP_METHOD_PUT,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        mbedtls_sha256_free(&sha);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "image/jpeg");

    esp_err_t err = esp_http_client_open(client, (int)fb->len);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        mbedtls_sha256_free(&sha);
        return err;
    }

    /* Stream framebuffer in chunks */
    const uint8_t *buf  = fb->buf;
    size_t remaining    = fb->len;
    const size_t CHUNK  = 4096;

    while (remaining > 0) {
        size_t to_send = remaining < CHUNK ? remaining : CHUNK;
        int written = esp_http_client_write(client, (const char *)buf, (int)to_send);
        if (written <= 0) {
            esp_http_client_cleanup(client);
            mbedtls_sha256_free(&sha);
            return ESP_FAIL;
        }
        mbedtls_sha256_update(&sha, buf, (size_t)written);
        buf       += written;
        remaining -= (size_t)written;
    }

    mbedtls_sha256_finish(&sha, sha_out);
    mbedtls_sha256_free(&sha);

    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    return (status >= 200 && status < 300) ? ESP_OK : ESP_FAIL;
}

/* ------------------------------------------------------------------
 * Perform one upload job (runs in the dedicated upload task).
 *
 * Does the blocking HTTPS PUT, publishes media/complete on success, then
 * releases the framebuffer and returns the state machine to IDLE.
 * ------------------------------------------------------------------ */

static void camera_run_upload(const cam_upload_job_t *job)
{
    uint8_t sha_bytes[32];
    esp_err_t err = camera_do_upload(job->url, job->fb, sha_bytes);

    if (err == ESP_OK) {
        /* Format sha256 as lowercase hex */
        char sha_hex[65];
        for (int i = 0; i < 32; i++) {
            snprintf(sha_hex + i * 2, 3, "%02x", sha_bytes[i]);
        }

        char topic_buf[JETTYD_MQTT_MAX_TOPIC];
        jettyd_mqtt_build_topic(topic_buf, sizeof(topic_buf), "media/complete");

        char payload[256];
        snprintf(payload, sizeof(payload),
                 "{\"id\":\"%s\",\"sha256\":\"%s\",\"status\":\"ok\",\"size\":%zu}",
                 job->request_id, sha_hex, job->fb_len);

        jettyd_mqtt_publish(topic_buf, payload, 1, false);
        ESP_LOGI(TAG, "Upload complete id=%s sha256=%.8s...", job->request_id, sha_hex);
    }

    /* Release framebuffer and return to IDLE */
    portENTER_CRITICAL(&s_state_mux);
    s_fb           = NULL;
    s_upload_state = CAM_STATE_IDLE;
    portEXIT_CRITICAL(&s_state_mux);

    esp_camera_fb_return(job->fb);

    if (err != ESP_OK) {
        s_upload_errors++;
        camera_push_error_to_shadow();
        ESP_LOGW(TAG, "PUT to presigned URL failed");
    }
}

/* ------------------------------------------------------------------
 * Dedicated upload task — drains the upload queue and performs the
 * blocking transfer off the MQTT event task.
 * ------------------------------------------------------------------ */

static void camera_upload_task(void *arg)
{
    (void)arg;
    cam_upload_job_t job;
    for (;;) {
        if (xQueueReceive(s_upload_queue, &job, portMAX_DELAY) == pdTRUE) {
            camera_run_upload(&job);
        }
    }
}

/* ------------------------------------------------------------------
 * State machine: capture trigger
 * ------------------------------------------------------------------ */

static esp_err_t camera_trigger_capture(void)
{
    /* Quota backoff */
    if (s_quota_retry_at > 0 && esp_timer_get_time() < s_quota_retry_at) {
        ESP_LOGW(TAG, "Quota backoff active, skipping capture");
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_state_mux);
    bool busy = (s_upload_state != CAM_STATE_IDLE);
    portEXIT_CRITICAL(&s_state_mux);
    if (busy) {
        ESP_LOGW(TAG, "Upload already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    s_fb = esp_camera_fb_get();
    if (!s_fb) {
        ESP_LOGW(TAG, "esp_camera_fb_get() returned NULL");
        return ESP_FAIL;
    }

    snprintf(s_request_id, sizeof(s_request_id), "%08" PRIu32, ++s_req_counter);

    char topic[JETTYD_MQTT_MAX_TOPIC];
    jettyd_mqtt_build_topic(topic, sizeof(topic), "media/request");

    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"id\":\"%s\",\"size\":%zu,\"format\":\"jpeg\"}",
             s_request_id, s_fb->len);

    jettyd_mqtt_publish(topic, payload, 1, false);

    portENTER_CRITICAL(&s_state_mux);
    s_upload_state = CAM_STATE_AWAIT_GRANT;
    portEXIT_CRITICAL(&s_state_mux);

    s_grant_deadline = esp_timer_get_time() +
                       (int64_t)s_cfg.grant_timeout_sec * 1000000LL;

    /* Start one-shot grant timeout timer */
    if (s_grant_timeout_timer) {
        xTimerChangePeriod(s_grant_timeout_timer,
                           pdMS_TO_TICKS((uint32_t)s_cfg.grant_timeout_sec * 1000UL), 0);
        xTimerStart(s_grant_timeout_timer, 0);
    }

    ESP_LOGI(TAG, "Capture triggered id=%s size=%zu timeout=%" PRIu32 "s",
             s_request_id, s_fb->len, s_cfg.grant_timeout_sec);
    return ESP_OK;
}

/* ------------------------------------------------------------------
 * State machine: grant timeout callback (FreeRTOS timer daemon)
 *
 * xTimerStop does NOT cancel a callback already dispatched to the timer
 * daemon queue, so this may fire even after a grant arrived. It claims the
 * IDLE transition atomically and only acts when still AWAIT_GRANT — so it
 * can never release a framebuffer the upload task now owns (UPLOADING).
 * ------------------------------------------------------------------ */

static void camera_grant_timeout_cb(TimerHandle_t timer)
{
    (void)timer;

    portENTER_CRITICAL(&s_state_mux);
    bool act = (s_upload_state == CAM_STATE_AWAIT_GRANT);
    camera_fb_t *fb = NULL;
    if (act) {
        fb             = s_fb;
        s_fb           = NULL;
        s_upload_state = CAM_STATE_IDLE;
    }
    portEXIT_CRITICAL(&s_state_mux);

    if (!act) return;   /* grant already claimed the transition — no-op */

    if (fb) esp_camera_fb_return(fb);
    s_upload_errors++;
    camera_push_error_to_shadow();
    ESP_LOGW(TAG, "Camera upload failed: grant timeout");
}

/* ------------------------------------------------------------------
 * State machine: MQTT callback for media/grant
 *
 * Runs on the esp-mqtt client task. Must return quickly — it only
 * parses/validates the grant and hands the presigned URL to the upload
 * task. The blocking HTTPS PUT happens in camera_upload_task.
 * ------------------------------------------------------------------ */

static void camera_on_grant_received(const char *topic,
                                     const char *data, int data_len)
{
    (void)topic;
    if (data_len <= 0) return;

    portENTER_CRITICAL(&s_state_mux);
    bool awaiting = (s_upload_state == CAM_STATE_AWAIT_GRANT);
    portEXIT_CRITICAL(&s_state_mux);
    if (!awaiting) return;   /* late grant / not our request — ignore */

    /* Stop the timeout timer (best-effort; the state guard handles the race). */
    if (s_grant_timeout_timer) xTimerStop(s_grant_timeout_timer, 0);

    /* Quota exceeded? */
    if (strstr(data, "quota_exceeded") != NULL) {
        uint32_t retry_sec = 60;
        const char *p = strstr(data, "\"retry_after_sec\":");
        if (p) retry_sec = (uint32_t)atoi(p + 18);

        ESP_LOGW(TAG, "Quota exceeded, retry after %" PRIu32 " s", retry_sec);
        s_quota_retry_at = esp_timer_get_time() +
                           (int64_t)retry_sec * 1000000LL;

        portENTER_CRITICAL(&s_state_mux);
        camera_fb_t *fb = s_fb;
        s_fb           = NULL;
        s_upload_state = CAM_STATE_IDLE;
        portEXIT_CRITICAL(&s_state_mux);
        if (fb) esp_camera_fb_return(fb);
        return;
    }

    /* Extract presigned URL */
    const char *url_key   = "\"url\":\"";
    const char *url_start = strstr(data, url_key);
    if (!url_start) {
        camera_release_fb_and_fail("grant missing url");
        return;
    }
    url_start += strlen(url_key);
    const char *url_end = strchr(url_start, '"');
    if (!url_end) {
        camera_release_fb_and_fail("grant url malformed");
        return;
    }

    cam_upload_job_t job;
    memset(&job, 0, sizeof(job));
    size_t url_len = (size_t)(url_end - url_start);
    if (url_len >= sizeof(job.url)) url_len = sizeof(job.url) - 1;
    memcpy(job.url, url_start, url_len);
    job.url[url_len] = '\0';
    strlcpy(job.request_id, s_request_id, sizeof(job.request_id));

    /* Transition to UPLOADING and take ownership of the framebuffer under the
     * mux, then hand off. A timeout callback racing us is now a no-op. */
    portENTER_CRITICAL(&s_state_mux);
    if (s_upload_state != CAM_STATE_AWAIT_GRANT) {
        portEXIT_CRITICAL(&s_state_mux);
        return;                    /* timeout won the race — nothing to do */
    }
    s_upload_state = CAM_STATE_UPLOADING;
    job.fb     = s_fb;
    job.fb_len = s_fb ? s_fb->len : 0;
    portEXIT_CRITICAL(&s_state_mux);

    if (s_upload_queue == NULL ||
        xQueueSend(s_upload_queue, &job, 0) != pdTRUE) {
        camera_release_fb_and_fail("upload queue full");
    }
}

/* ------------------------------------------------------------------
 * Interval timer callback
 * ------------------------------------------------------------------ */

static void __attribute__((unused)) camera_interval_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    camera_trigger_capture();
}

/* ------------------------------------------------------------------
 * MCP tool: camera_capture (non-blocking, enqueues a capture)
 * ------------------------------------------------------------------ */

static esp_err_t camera_mcp_capture(const char *params_json,
                                    char *out, size_t out_len)
{
    (void)params_json;
    esp_err_t err = camera_trigger_capture();
    if (err == ESP_OK) {
        snprintf(out, out_len, "{\"status\":\"capture_queued\"}");
    } else {
        snprintf(out, out_len, "{\"status\":\"error\",\"code\":%d}", (int)err);
    }
    return err;
}

/* ------------------------------------------------------------------
 * Hardware init
 * ------------------------------------------------------------------ */

static esp_err_t camera_hw_init(void)
{
    if (!esp_psram_is_initialized()) {
        ESP_LOGE(TAG, "PSRAM not available — camera driver requires PSRAM");
        return ESP_ERR_NOT_FOUND;
    }

    camera_config_t hw_cfg = s_ov2640_s3cam_preset;
    hw_cfg.frame_size   = camera_map_frame_size(s_cfg.frame_size);
    hw_cfg.jpeg_quality = (int)s_cfg.jpeg_quality;

    esp_err_t err = esp_camera_init(&hw_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: %s", esp_err_to_name(err));
    }
    return err;
}

/* ------------------------------------------------------------------
 * Public: camera_register
 * ------------------------------------------------------------------ */

void camera_register(const char *instance, const void *config)
{
    const camera_driver_config_t *c = (const camera_driver_config_t *)config;
    s_cfg = *c;
    camera_config_validate(&s_cfg);

    strlcpy(s_instance, instance, sizeof(s_instance));

    esp_err_t err = camera_hw_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera HW init failed (%s), driver not registered",
                 esp_err_to_name(err));
        return;
    }

    /* Upload queue + dedicated upload task (keeps the blocking PUT off the
     * MQTT event task). */
    s_upload_queue = xQueueCreate(2, sizeof(cam_upload_job_t));
    if (s_upload_queue) {
        xTaskCreate(camera_upload_task, "cam_upload", 8192, NULL, 5, &s_upload_task);
    } else {
        ESP_LOGE(TAG, "Failed to create camera upload queue");
    }

    /* Subscribe to media/grant */
    char grant_topic[JETTYD_MQTT_MAX_TOPIC];
    jettyd_mqtt_build_topic(grant_topic, sizeof(grant_topic), "media/grant");
    jettyd_mqtt_subscribe(grant_topic, 1, camera_on_grant_received);

    /* Grant timeout timer (one-shot, not started until capture triggers) */
    s_grant_timeout_timer = xTimerCreate(
        "cam_grant_to",
        pdMS_TO_TICKS(s_cfg.grant_timeout_sec * 1000UL),
        pdFALSE, NULL, camera_grant_timeout_cb);

    /* Interval timer */
    if (s_cfg.capture_interval_sec > 0) {
        s_interval_timer = xTimerCreate(
            "cam_interval",
            pdMS_TO_TICKS(s_cfg.capture_interval_sec * 1000UL),
            pdTRUE, NULL, camera_interval_timer_cb);
        if (s_interval_timer) xTimerStart(s_interval_timer, 0);
        ESP_LOGI(TAG, "Interval capture every %" PRIu32 " s",
                 s_cfg.capture_interval_sec);
    }

    /* Register driver struct */
    memset(&s_driver, 0, sizeof(s_driver));
    strlcpy(s_driver.instance,    instance, JETTYD_MAX_INSTANCE_NAME - 1);
    strlcpy(s_driver.driver_name, "camera", sizeof(s_driver.driver_name));

    s_driver.capability_count = 0; /* event-driven, not polled */

    static const char *schema_capture =
        "{\"type\":\"object\",\"properties\":{}}";

    strlcpy(s_driver.mcp_tools[0].name, "camera_capture",
            sizeof(s_driver.mcp_tools[0].name));
    s_driver.mcp_tools[0].description       =
        "Capture a JPEG image and upload via the media grant flow";
    s_driver.mcp_tools[0].input_schema_json = schema_capture;
    s_driver.mcp_tools[0].handler           = camera_mcp_capture;
    s_driver.mcp_tool_count = 1;

    JETTYD_REGISTER_DRIVER(&s_driver);

    ESP_LOGI(TAG, "Camera registered: instance=%s frame_size=%d quality=%u "
             "interval=%" PRIu32 "s grant_timeout=%" PRIu32 "s",
             instance, (int)s_cfg.frame_size, s_cfg.jpeg_quality,
             s_cfg.capture_interval_sec, s_cfg.grant_timeout_sec);
}

#else /* ============================================================
 * No-op stub — non-camera targets (esp32c3, etc.)
 * ============================================================ */

void camera_register(const char *instance, const void *config)
{
    (void)instance; (void)config;
    ESP_LOGI(TAG, "camera: unsupported on this target (no camera interface)");
}

#endif /* CONFIG_JETTYD_CAMERA_SUPPORTED */
