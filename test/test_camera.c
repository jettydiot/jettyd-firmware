/**
 * @file test_camera.c
 * @brief Host-stub unit tests for the camera driver (FLU-157).
 *
 * Covers:
 *  A. Config validation: sensor rejection, jpeg_quality clamping
 *  B. Upload state machine: request → grant → complete, grant timeout
 *  C. Quota backoff: retry_after_sec suppresses new requests
 *  D. PSRAM-absent graceful failure (simulates non-camera target behaviour)
 *
 * White-box pattern: includes camera.c directly so static helpers are
 * accessible, matching the approach used in test_manifest.c / test_mcp_tools.c.
 *
 * TDD note: this file was written before the driver implementation existed.
 * Tests verified failing with a stub camera.c, then made green by the full
 * implementation.
 */

/* ── Pull in stubs and esp_camera mock early so camera_fb_t is known ── */

#include "mocks/esp_idf_stubs.h"
#include "mocks/esp_camera.h"   /* defines camera_fb_t, camera_config_t (real HW struct) */
#include "mocks/esp_psram.h"
#include "mocks/esp_http_client.h"
#include "mocks/freertos/queue.h"
#include "mocks/mbedtls/sha256.h"
#include "jettyd_driver.h"

/* ── Camera-mock control globals (extern in mock headers) ─────────────── */

static uint8_t g_test_fb_data[512];
static camera_fb_t g_test_fb_instance;

camera_fb_t *g_test_fb        = NULL;
bool         g_camera_init_ok = true;
int          g_fb_return_count = 0;
int          g_camera_init_frame_size  = -1;
int          g_camera_init_fb_location = -1;
int          g_fb_get_count    = 0;

/* AE/AWB settle sequence-mode frames (FLU-157) */
bool         g_fb_seq_mode = false;
camera_fb_t  g_fb_seq_frames[MOCK_FB_SEQ_MAX];
uint8_t      g_fb_seq_data[512];

bool   g_psram_available     = true;

int    g_http_status_code    = 200;
size_t g_http_bytes_written  = 0;
bool   g_http_open_ok        = true;
char   g_http_last_url[512]  = {0};

/* Fixed SHA-256 output for test assertions */
uint8_t g_fake_sha256[32] = {
    0xde, 0xad, 0xbe, 0xef, 0xca, 0xfe, 0xba, 0xbe,
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
};
/* Hex of g_fake_sha256[0..3]: "deadbeef" */

/* ── MQTT stubs ─────────────────────────────────────────────────────── */

#include "jettyd_mqtt.h"

static bool  s_publish_called                         = false;
static char  s_published_topic[JETTYD_MQTT_MAX_TOPIC] = {0};
static char  s_published_payload[1024]                = {0};
static bool  s_subscribe_called                       = false;
static char  s_subscribed_topic[JETTYD_MQTT_MAX_TOPIC]= {0};
static int   s_publish_count                          = 0;

bool jettyd_mqtt_is_connected(void) { return true; }

esp_err_t jettyd_mqtt_build_topic(char *buf, size_t buf_len, const char *suffix)
{
    int w = snprintf(buf, buf_len, "jettyd/tenant/devkey/%s", suffix);
    if (w < 0 || (size_t)w >= buf_len) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

esp_err_t jettyd_mqtt_publish(const char *topic, const char *data,
                               uint8_t qos, bool retain)
{
    (void)qos; (void)retain;
    s_publish_called = true;
    s_publish_count++;
    strncpy(s_published_topic,   topic, sizeof(s_published_topic)   - 1);
    strncpy(s_published_payload, data,  sizeof(s_published_payload) - 1);
    return ESP_OK;
}

esp_err_t jettyd_mqtt_subscribe(const char *topic, uint8_t qos,
                                 jettyd_mqtt_msg_cb_t cb)
{
    (void)qos; (void)cb;
    s_subscribe_called = true;
    strncpy(s_subscribed_topic, topic, sizeof(s_subscribed_topic) - 1);
    return ESP_OK;
}

esp_err_t jettyd_mqtt_init(const jettyd_mqtt_config_t *c)       { (void)c; return ESP_OK; }
esp_err_t jettyd_mqtt_connect(void)                              { return ESP_OK; }
esp_err_t jettyd_mqtt_disconnect(void)                           { return ESP_OK; }
esp_err_t jettyd_mqtt_reconfigure(const char *u, const char *p) { (void)u;(void)p; return ESP_OK; }
esp_err_t jettyd_mqtt_flush_buffer(void)                         { return ESP_OK; }

/* ── Shadow stub ────────────────────────────────────────────────────── */

#include "jettyd_shadow.h"

static char   s_shadow_key[64]   = {0};
static int32_t s_shadow_int_val  = -1;
static bool    s_shadow_called   = false;

esp_err_t jettyd_shadow_update(const char *dotted_name, jettyd_value_t value)
{
    s_shadow_called = true;
    strncpy(s_shadow_key, dotted_name, sizeof(s_shadow_key) - 1);
    if (value.type == JETTYD_VAL_INT) s_shadow_int_val = value.int_val;
    return ESP_OK;
}

esp_err_t jettyd_shadow_init(void)                              { return ESP_OK; }
esp_err_t jettyd_shadow_update_system(const char *k, jettyd_value_t v) { (void)k;(void)v; return ESP_OK; }
esp_err_t jettyd_shadow_set_desired(const char *j, int l)      { (void)j;(void)l; return ESP_OK; }
esp_err_t jettyd_shadow_publish(void)                           { return ESP_OK; }
esp_err_t jettyd_shadow_persist(void)                           { return ESP_OK; }
esp_err_t jettyd_shadow_reconcile(void)                         { return ESP_OK; }
void      jettyd_shadow_desired_handler(const char *t, const char *d, int l) { (void)t;(void)d;(void)l; }
int       jettyd_shadow_serialize(char *b, size_t n)            { (void)b;(void)n; return 0; }

/* ── Include unit under test ────────────────────────────────────────── */

#ifndef CONFIG_JETTYD_CAMERA_SUPPORTED
#define CONFIG_JETTYD_CAMERA_SUPPORTED 1
#endif
#include "../drivers/camera/camera.c"

/* ── Provision stubs (needed by driver_registry.c via JETTYD_REGISTER_DRIVER) ── */
/* driver_registry.c doesn't call provision functions, so none needed. */

/* ── Test framework ─────────────────────────────────────────────────── */

static int s_passed = 0;
static int s_failed = 0;

#define TEST(name) static void test_##name(void)

/* Reset all mutable state between tests */
static void camera_test_reset(void)
{
    /* Camera driver static state (accessible because we included camera.c) */
    s_upload_state    = CAM_STATE_IDLE;
    s_fb              = NULL;
    s_grant_deadline  = 0;
    s_quota_retry_at  = 0;
    s_upload_errors   = 0;
    s_req_counter     = 0;
    memset(s_request_id, 0, sizeof(s_request_id));
    memset(s_instance, 0, sizeof(s_instance));

    /* MQTT stubs */
    s_publish_called  = false;
    s_publish_count   = 0;
    s_subscribe_called = false;
    memset(s_published_topic,   0, sizeof(s_published_topic));
    memset(s_published_payload, 0, sizeof(s_published_payload));
    memset(s_subscribed_topic,  0, sizeof(s_subscribed_topic));

    /* Shadow stubs */
    s_shadow_called   = false;
    s_shadow_int_val  = -1;
    memset(s_shadow_key, 0, sizeof(s_shadow_key));

    /* Camera mock */
    g_test_fb          = NULL;
    g_camera_init_ok   = true;
    g_fb_return_count  = 0;
    g_camera_init_frame_size  = -1;
    g_camera_init_fb_location = -1;
    g_fb_get_count     = 0;
    g_fb_seq_mode      = false;
    g_psram_available  = true;

    /* HTTP mock */
    g_http_status_code   = 200;
    g_http_bytes_written = 0;
    g_http_open_ok       = true;
    memset(g_http_last_url, 0, sizeof(g_http_last_url));

    /* Fresh upload queue for each test (the dedicated upload task is a no-op
     * on the host stub, so tests drain the queue synchronously via
     * pump_upload_queue()). */
    s_upload_queue = xQueueCreate(4, sizeof(cam_upload_job_t));

    /* Set up a default driver config */
    s_cfg.sensor               = CAMERA_SENSOR_OV2640;
    s_cfg.frame_size           = CAMERA_FRAME_SVGA;
    s_cfg.jpeg_quality         = 12;
    s_cfg.capture_interval_sec = 0;
    s_cfg.grant_timeout_sec    = 30;
    strlcpy(s_instance, "cam", sizeof(s_instance));
}

/* Drain the upload queue synchronously, running each job through the same
 * camera_run_upload() the dedicated upload task uses on-device. */
static void pump_upload_queue(void)
{
    cam_upload_job_t job;
    while (xQueueReceive(s_upload_queue, &job, 0) == pdTRUE) {
        camera_run_upload(&job);
    }
}

static void init_test_fb(void)
{
    memset(g_test_fb_data, 0xAB, sizeof(g_test_fb_data));
    g_test_fb_data[0] = 0xFF; g_test_fb_data[1] = 0xD8; /* JPEG SOI */
    g_test_fb_instance.buf    = g_test_fb_data;
    g_test_fb_instance.len    = sizeof(g_test_fb_data);
    g_test_fb_instance.width  = 320;
    g_test_fb_instance.height = 240;
    g_test_fb_instance.format = PIXFORMAT_JPEG;
    g_test_fb = &g_test_fb_instance;
}

#define RUN_TEST(name) do { \
    printf("\n--- Test: %s ---\n", #name); \
    jettyd_driver_registry_init(); \
    camera_test_reset(); \
    test_##name(); \
    printf("--- %s: PASSED ---\n", #name); \
    s_passed++; \
} while(0)

#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        s_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_EQ(a, b)   ASSERT_TRUE((a) == (b))
#define ASSERT_STR_EQ(a, b)  ASSERT_TRUE(strcmp((a),(b)) == 0)
#define ASSERT_STR_CONTAINS(hay, needle) \
    ASSERT_TRUE(strstr((hay), (needle)) != NULL)
#define ASSERT_STR_NOT_CONTAINS(hay, needle) \
    ASSERT_TRUE(strstr((hay), (needle)) == NULL)

/* ══════════════════════════════════════════════════════════════════════
 * Group A: Config validation
 * ══════════════════════════════════════════════════════════════════════ */

TEST(config_rejects_unknown_sensor) {
    camera_driver_config_t cfg = {
        .sensor               = CAMERA_SENSOR_UNKNOWN,
        .frame_size           = CAMERA_FRAME_SVGA,
        .jpeg_quality         = 12,
        .capture_interval_sec = 0,
        .grant_timeout_sec    = 30,
    };
    esp_err_t err = camera_config_validate(&cfg);
    ASSERT_EQ(err, ESP_ERR_INVALID_ARG);
}

TEST(config_accepts_ov2640) {
    camera_driver_config_t cfg = {
        .sensor               = CAMERA_SENSOR_OV2640,
        .frame_size           = CAMERA_FRAME_SVGA,
        .jpeg_quality         = 10,
        .capture_interval_sec = 0,
        .grant_timeout_sec    = 30,
    };
    esp_err_t err = camera_config_validate(&cfg);
    ASSERT_EQ(err, ESP_OK);
}

TEST(config_clamps_jpeg_quality_to_63) {
    camera_driver_config_t cfg = {
        .sensor               = CAMERA_SENSOR_OV2640,
        .frame_size           = CAMERA_FRAME_VGA,
        .jpeg_quality         = 200,
        .capture_interval_sec = 0,
        .grant_timeout_sec    = 30,
    };
    esp_err_t err = camera_config_validate(&cfg);
    ASSERT_EQ(err, ESP_OK);
    ASSERT_EQ(cfg.jpeg_quality, 63);
}

TEST(config_quality_at_63_not_clamped) {
    camera_driver_config_t cfg = {
        .sensor               = CAMERA_SENSOR_OV2640,
        .frame_size           = CAMERA_FRAME_VGA,
        .jpeg_quality         = 63,
        .capture_interval_sec = 0,
        .grant_timeout_sec    = 30,
    };
    camera_config_validate(&cfg);
    ASSERT_EQ(cfg.jpeg_quality, 63);
}

TEST(config_fills_default_grant_timeout) {
    camera_driver_config_t cfg = {
        .sensor               = CAMERA_SENSOR_OV2640,
        .frame_size           = CAMERA_FRAME_SVGA,
        .jpeg_quality         = 12,
        .capture_interval_sec = 0,
        .grant_timeout_sec    = 0, /* should be filled with default 30 */
    };
    camera_config_validate(&cfg);
    ASSERT_EQ(cfg.grant_timeout_sec, 30u);
}

/* ══════════════════════════════════════════════════════════════════════
 * Group B: Upload state machine
 * ══════════════════════════════════════════════════════════════════════ */

TEST(trigger_publishes_media_request) {
    init_test_fb();
    esp_err_t err = camera_trigger_capture();
    ASSERT_EQ(err, ESP_OK);
    ASSERT_TRUE(s_publish_called);
    ASSERT_STR_CONTAINS(s_published_topic, "media/request");
}

TEST(trigger_sets_state_to_await_grant) {
    init_test_fb();
    camera_trigger_capture();
    ASSERT_EQ(s_upload_state, CAM_STATE_AWAIT_GRANT);
    ASSERT_TRUE(s_fb != NULL);
}

TEST(media_request_payload_contains_id_and_size) {
    init_test_fb();
    camera_trigger_capture();
    ASSERT_STR_CONTAINS(s_published_payload, "\"id\":");
    ASSERT_STR_CONTAINS(s_published_payload, "\"size\":");
    ASSERT_STR_CONTAINS(s_published_payload, "\"format\":\"jpeg\"");
}

TEST(grant_triggers_upload_and_publishes_complete) {
    init_test_fb();
    g_http_status_code = 200;
    g_http_open_ok     = true;

    camera_trigger_capture();

    /* Reset publish tracking so we can detect media/complete separately */
    s_publish_called = false;
    memset(s_published_topic, 0, sizeof(s_published_topic));
    memset(s_published_payload, 0, sizeof(s_published_payload));

    const char *grant = "{\"url\":\"https://upload.example.com/put?sig=abc\"}";
    camera_on_grant_received("jettyd/tenant/devkey/media/grant",
                              grant, (int)strlen(grant));
    /* Grant only hands off; the upload task publishes media/complete. */
    pump_upload_queue();

    ASSERT_TRUE(s_publish_called);
    ASSERT_STR_CONTAINS(s_published_topic, "media/complete");
    ASSERT_STR_CONTAINS(s_published_payload, "\"status\":\"ok\"");
    ASSERT_STR_CONTAINS(s_published_payload, "\"sha256\":");
    /* Our fixed sha256 starts with "deadbeef" */
    ASSERT_STR_CONTAINS(s_published_payload, "deadbeef");
}

TEST(grant_releases_framebuffer_on_success) {
    init_test_fb();
    camera_trigger_capture();

    const char *grant = "{\"url\":\"https://upload.example.com/put?sig=abc\"}";
    camera_on_grant_received("grant", grant, (int)strlen(grant));
    pump_upload_queue();

    ASSERT_EQ(g_fb_return_count, 1);
    ASSERT_TRUE(s_fb == NULL);
    ASSERT_EQ(s_upload_state, CAM_STATE_IDLE);
}

TEST(grant_upload_streams_full_framebuffer) {
    init_test_fb();
    g_http_open_ok     = true;
    camera_trigger_capture();

    const char *grant = "{\"url\":\"https://s3.example.com/upload\"}";
    camera_on_grant_received("grant", grant, (int)strlen(grant));
    pump_upload_queue();

    /* All 512 bytes of the fake framebuffer should have been written */
    ASSERT_EQ(g_http_bytes_written, sizeof(g_test_fb_data));
}

TEST(grant_timeout_releases_fb_and_increments_errors) {
    init_test_fb();
    camera_trigger_capture();

    /* Sanity: we're in await-grant state with a live fb */
    ASSERT_EQ(s_upload_state, CAM_STATE_AWAIT_GRANT);
    ASSERT_TRUE(s_fb != NULL);
    uint32_t errors_before = s_upload_errors;

    /* Simulate timeout by calling the timer callback directly */
    camera_grant_timeout_cb(NULL);

    ASSERT_EQ(s_upload_state, CAM_STATE_IDLE);
    ASSERT_TRUE(s_fb == NULL);
    ASSERT_EQ(g_fb_return_count, 1);
    ASSERT_EQ(s_upload_errors, errors_before + 1);
}

TEST(grant_timeout_updates_shadow_error_counter) {
    init_test_fb();
    camera_trigger_capture();
    camera_grant_timeout_cb(NULL);

    ASSERT_TRUE(s_shadow_called);
    ASSERT_STR_CONTAINS(s_shadow_key, "upload_errors");
    ASSERT_EQ(s_shadow_int_val, 1);
}

TEST(put_failure_increments_error_counter) {
    init_test_fb();
    g_http_status_code = 503;  /* upload fails */
    camera_trigger_capture();

    s_shadow_called = false;
    const char *grant = "{\"url\":\"https://upload.example.com/put\"}";
    camera_on_grant_received("grant", grant, (int)strlen(grant));
    pump_upload_queue();

    ASSERT_EQ(g_fb_return_count, 1);
    ASSERT_EQ(s_upload_state, CAM_STATE_IDLE);
    ASSERT_TRUE(s_upload_errors > 0);
    ASSERT_TRUE(s_shadow_called);
}

TEST(put_failure_does_not_publish_complete) {
    init_test_fb();
    g_http_status_code = 500;
    camera_trigger_capture();

    s_publish_called = false;
    const char *grant = "{\"url\":\"https://upload.example.com/put\"}";
    camera_on_grant_received("grant", grant, (int)strlen(grant));
    pump_upload_queue();

    ASSERT_TRUE(!s_publish_called || !strstr(s_published_topic, "complete"));
}

TEST(grant_without_url_field_is_failure) {
    init_test_fb();
    camera_trigger_capture();
    uint32_t errors_before = s_upload_errors;

    const char *bad_grant = "{\"status\":\"ok\"}"; /* no url field */
    camera_on_grant_received("grant", bad_grant, (int)strlen(bad_grant));

    ASSERT_EQ(s_upload_state, CAM_STATE_IDLE);
    ASSERT_EQ(s_upload_errors, errors_before + 1);
}

TEST(idle_state_ignores_grant) {
    /* No capture triggered; grant should be ignored */
    ASSERT_EQ(s_upload_state, CAM_STATE_IDLE);
    int count_before = s_publish_count;

    const char *grant = "{\"url\":\"https://upload.example.com/put\"}";
    camera_on_grant_received("grant", grant, (int)strlen(grant));

    ASSERT_EQ(s_publish_count, count_before);
    ASSERT_EQ(s_upload_state, CAM_STATE_IDLE);
}

/* ══════════════════════════════════════════════════════════════════════
 * Group B2: Grant/timeout race (use-after-free regression, FLU-157 review)
 * ══════════════════════════════════════════════════════════════════════ */

/* A grant that arrives AFTER the timeout callback has already fired must be
 * ignored — it must not release the framebuffer a second time (UAF) nor
 * enqueue a stale upload. */
TEST(late_grant_after_timeout_does_not_double_release) {
    init_test_fb();
    camera_trigger_capture();
    ASSERT_EQ(s_upload_state, CAM_STATE_AWAIT_GRANT);

    /* Timeout fires first: releases fb once, returns to IDLE. */
    camera_grant_timeout_cb(NULL);
    ASSERT_EQ(s_upload_state, CAM_STATE_IDLE);
    ASSERT_TRUE(s_fb == NULL);
    ASSERT_EQ(g_fb_return_count, 1);
    uint32_t errors_after_timeout = s_upload_errors;

    /* Late grant arrives — state is no longer AWAIT_GRANT, so it is a no-op. */
    const char *grant = "{\"url\":\"https://upload.example.com/put\"}";
    camera_on_grant_received("grant", grant, (int)strlen(grant));
    pump_upload_queue();   /* nothing should have been queued */

    ASSERT_EQ(g_fb_return_count, 1);                 /* NOT 2 — no double free */
    ASSERT_EQ(s_upload_state, CAM_STATE_IDLE);
    ASSERT_EQ(s_upload_errors, errors_after_timeout); /* late grant added no error */
}

/* Conversely: once a grant has handed off to the upload task (UPLOADING), a
 * stale timeout callback must NOT touch the framebuffer the task is streaming. */
TEST(timeout_after_grant_handoff_is_noop) {
    init_test_fb();
    camera_trigger_capture();

    const char *grant = "{\"url\":\"https://upload.example.com/put\"}";
    camera_on_grant_received("grant", grant, (int)strlen(grant));

    /* Handed off, not yet uploaded: state UPLOADING, fb still owned. */
    ASSERT_EQ(s_upload_state, CAM_STATE_UPLOADING);
    ASSERT_TRUE(s_fb != NULL);

    /* Stale timeout callback fires mid-flight — must not release the fb. */
    camera_grant_timeout_cb(NULL);
    ASSERT_EQ(g_fb_return_count, 0);
    ASSERT_EQ(s_upload_state, CAM_STATE_UPLOADING);

    /* Upload task then runs and releases the fb exactly once. */
    pump_upload_queue();
    ASSERT_EQ(g_fb_return_count, 1);
    ASSERT_EQ(s_upload_state, CAM_STATE_IDLE);
}

/* The grant callback must return quickly: it enqueues rather than uploading
 * inline, so no media/complete is published until the upload task drains it. */
TEST(grant_hands_off_without_blocking_publish) {
    init_test_fb();
    camera_trigger_capture();

    s_publish_called = false;
    memset(s_published_topic, 0, sizeof(s_published_topic));
    const char *grant = "{\"url\":\"https://upload.example.com/put\"}";
    camera_on_grant_received("grant", grant, (int)strlen(grant));

    /* Before the upload task runs: no media/complete yet, fb still held. */
    ASSERT_TRUE(!s_publish_called);
    ASSERT_EQ(s_upload_state, CAM_STATE_UPLOADING);
    ASSERT_EQ(g_http_bytes_written, 0u);

    pump_upload_queue();
    ASSERT_STR_CONTAINS(s_published_topic, "media/complete");
}

/* ══════════════════════════════════════════════════════════════════════
 * Group C: Quota backoff
 * ══════════════════════════════════════════════════════════════════════ */

TEST(quota_exceeded_releases_fb_returns_to_idle) {
    init_test_fb();
    camera_trigger_capture();

    const char *quota_grant =
        "{\"quota_exceeded\":true,\"retry_after_sec\":60}";
    camera_on_grant_received("grant", quota_grant, (int)strlen(quota_grant));

    ASSERT_EQ(s_upload_state, CAM_STATE_IDLE);
    ASSERT_TRUE(s_fb == NULL);
    ASSERT_EQ(g_fb_return_count, 1);
    /* Not an error: error counter should NOT have increased */
    ASSERT_EQ(s_upload_errors, 0u);
}

TEST(quota_backoff_suppresses_next_capture) {
    init_test_fb();
    camera_trigger_capture();

    /* Grant with quota exceeded, 60 s backoff */
    const char *quota_grant =
        "{\"quota_exceeded\":true,\"retry_after_sec\":60}";
    camera_on_grant_received("grant", quota_grant, (int)strlen(quota_grant));

    /* s_quota_retry_at is now set far in the future (60 s from now).
       Immediately try to capture again — should be blocked by backoff. */
    g_test_fb       = &g_test_fb_instance; /* re-arm fake fb */
    s_publish_count = 0;
    esp_err_t err   = camera_trigger_capture();

    ASSERT_EQ(err, ESP_ERR_INVALID_STATE);
    ASSERT_EQ(s_publish_count, 0); /* no media/request published */
}

TEST(quota_backoff_honours_retry_after_sec) {
    /* Set backoff to 1 second = 1,000,000 µs in the past (already expired) */
    s_quota_retry_at = esp_timer_get_time() - 1; /* expired */
    init_test_fb();

    esp_err_t err = camera_trigger_capture();

    /* Backoff has expired — capture should proceed */
    ASSERT_EQ(err, ESP_OK);
    ASSERT_EQ(s_upload_state, CAM_STATE_AWAIT_GRANT);
}

/* ══════════════════════════════════════════════════════════════════════
 * Group D: PSRAM-absent graceful failure / no-op path
 * ══════════════════════════════════════════════════════════════════════ */

TEST(psram_absent_init_returns_error) {
    g_psram_available = false;
    esp_err_t err = camera_hw_init();
    ASSERT_EQ(err, ESP_ERR_NOT_FOUND);
}

TEST(register_without_psram_does_not_crash) {
    g_psram_available = false;
    camera_driver_config_t cfg = {
        .sensor               = CAMERA_SENSOR_OV2640,
        .frame_size           = CAMERA_FRAME_SVGA,
        .jpeg_quality         = 12,
        .capture_interval_sec = 0,
        .grant_timeout_sec    = 30,
    };
    /* camera_register should log error and return cleanly */
    camera_register("cam", &cfg);
    /* Driver should NOT have been registered (registry still empty) */
    ASSERT_EQ(jettyd_driver_count(), 0);
}

/* ══════════════════════════════════════════════════════════════════════
 * Group E: MCP tool
 * ══════════════════════════════════════════════════════════════════════ */

TEST(mcp_capture_returns_queued_on_success) {
    init_test_fb();
    char out[64];
    esp_err_t err = camera_mcp_capture(NULL, out, sizeof(out));
    ASSERT_EQ(err, ESP_OK);
    ASSERT_STR_CONTAINS(out, "capture_queued");
}

TEST(mcp_capture_publishes_request) {
    init_test_fb();
    char out[64];
    camera_mcp_capture(NULL, out, sizeof(out));
    ASSERT_TRUE(s_publish_called);
    ASSERT_STR_CONTAINS(s_published_topic, "media/request");
}

/* ══════════════════════════════════════════════════════════════════════
 * Group F: HW frame-size mapping (real esp32-camera framesize_t)
 *
 * The driver enum (QVGA=0..UXGA=4) is NOT numerically aligned with the real
 * framesize_t (QVGA=5, VGA=8, SVGA=9, XGA=10, UXGA=13). These tests fail if
 * anyone reintroduces a bare (int) cast instead of the mapping table.
 * ══════════════════════════════════════════════════════════════════════ */

TEST(hw_init_maps_svga_to_real_framesize) {
    g_psram_available = true;
    s_cfg.frame_size  = CAMERA_FRAME_SVGA;
    esp_err_t err = camera_hw_init();
    ASSERT_EQ(err, ESP_OK);
    ASSERT_EQ(g_camera_init_frame_size, FRAMESIZE_SVGA);   /* 9, not 2 */
    ASSERT_EQ(g_camera_init_fb_location, CAMERA_FB_IN_PSRAM);
}

TEST(hw_init_maps_qvga_to_real_framesize) {
    g_psram_available = true;
    s_cfg.frame_size  = CAMERA_FRAME_QVGA;
    camera_hw_init();
    ASSERT_EQ(g_camera_init_frame_size, FRAMESIZE_QVGA);   /* 5, not 0 */
}

TEST(hw_init_maps_uxga_to_real_framesize) {
    g_psram_available = true;
    s_cfg.frame_size  = CAMERA_FRAME_UXGA;
    camera_hw_init();
    ASSERT_EQ(g_camera_init_frame_size, FRAMESIZE_UXGA);   /* 13, not 4 */
}

/* ══════════════════════════════════════════════════════════════════════
 * Group G: AE/AWB settle frames (FLU-157 revision)
 *
 * The OV2640's first frames after sensor init are underexposed (pitch black);
 * auto-exposure/white-balance converge only after several frames. The driver
 * grabs and DISCARDS CONFIG_JETTYD_CAMERA_SETTLE_FRAMES frames after init so
 * the first frame it actually uploads is properly exposed.
 *
 * camera_settle(n) is exercised directly (camera_register calls it with the
 * Kconfig value after a successful hw_init).
 * ══════════════════════════════════════════════════════════════════════ */

/* Settle discards exactly N frames — each grabbed frame is returned. */
TEST(settle_discards_configured_frame_count) {
    init_test_fb();
    camera_settle(5);
    ASSERT_EQ(g_fb_get_count, 5);
    ASSERT_EQ(g_fb_return_count, 5);   /* every settle frame returned */
}

/* N == 0 disables settling: no frame is grabbed or discarded. */
TEST(settle_zero_frames_discards_nothing) {
    init_test_fb();
    camera_settle(0);
    ASSERT_EQ(g_fb_get_count, 0);
    ASSERT_EQ(g_fb_return_count, 0);
}

/* Settle frames are never uploaded: settling grabs+returns only, it publishes
 * nothing and leaves the state machine idle with no framebuffer held. The
 * subsequent capture obtains a FRESH frame for upload. */
TEST(settle_frames_are_not_uploaded) {
    g_fb_seq_mode = true;              /* distinct, seq-stamped frames */
    camera_settle(5);

    /* Settling alone: all 5 discarded, nothing published, nothing held. */
    ASSERT_EQ(g_fb_return_count, 5);
    ASSERT_TRUE(!s_publish_called);
    ASSERT_EQ(s_upload_state, CAM_STATE_IDLE);
    ASSERT_TRUE(s_fb == NULL);

    /* The upload frame is a fresh grab, distinct from every settle frame. */
    camera_trigger_capture();
    ASSERT_TRUE(s_fb != NULL);
    ASSERT_TRUE(s_fb->width > 5);      /* not one of settle frames 1..5 */
}

/* The uploaded frame is the (N+1)th frame grabbed from the sensor. */
TEST(upload_uses_frame_after_settle) {
    g_fb_seq_mode = true;
    camera_settle(5);                  /* grabs frames 1..5 (all discarded) */

    camera_trigger_capture();          /* grabs the 6th frame for upload */
    ASSERT_EQ(g_fb_get_count, 6);      /* N + 1 */
    ASSERT_TRUE(s_fb != NULL);
    ASSERT_EQ((int)s_fb->width, 6);    /* the (N+1)th frame is the upload frame */
    ASSERT_EQ(s_upload_state, CAM_STATE_AWAIT_GRANT);
}

/* Settling tolerates a transient NULL frame (fb_get miss) without stalling
 * or over-counting returns. */
TEST(settle_survives_null_frame) {
    g_test_fb = NULL;                  /* fb_get returns NULL every time */
    camera_settle(3);
    ASSERT_EQ(g_fb_get_count, 3);      /* still attempted N grabs */
    ASSERT_EQ(g_fb_return_count, 0);   /* nothing to return */
}

/* ══════════════════════════════════════════════════════════════════════
 * Main
 * ══════════════════════════════════════════════════════════════════════ */

int main(void)
{
    printf("═══════════════════════════════════════════\n");
    printf("  Jettyd Camera Driver Unit Tests (FLU-157)\n");
    printf("═══════════════════════════════════════════\n");

    /* Group A: Config validation */
    RUN_TEST(config_rejects_unknown_sensor);
    RUN_TEST(config_accepts_ov2640);
    RUN_TEST(config_clamps_jpeg_quality_to_63);
    RUN_TEST(config_quality_at_63_not_clamped);
    RUN_TEST(config_fills_default_grant_timeout);

    /* Group B: Upload state machine */
    RUN_TEST(trigger_publishes_media_request);
    RUN_TEST(trigger_sets_state_to_await_grant);
    RUN_TEST(media_request_payload_contains_id_and_size);
    RUN_TEST(grant_triggers_upload_and_publishes_complete);
    RUN_TEST(grant_releases_framebuffer_on_success);
    RUN_TEST(grant_upload_streams_full_framebuffer);
    RUN_TEST(grant_timeout_releases_fb_and_increments_errors);
    RUN_TEST(grant_timeout_updates_shadow_error_counter);
    RUN_TEST(put_failure_increments_error_counter);
    RUN_TEST(put_failure_does_not_publish_complete);
    RUN_TEST(grant_without_url_field_is_failure);
    RUN_TEST(idle_state_ignores_grant);

    /* Group B2: Grant/timeout race regressions */
    RUN_TEST(late_grant_after_timeout_does_not_double_release);
    RUN_TEST(timeout_after_grant_handoff_is_noop);
    RUN_TEST(grant_hands_off_without_blocking_publish);

    /* Group C: Quota backoff */
    RUN_TEST(quota_exceeded_releases_fb_returns_to_idle);
    RUN_TEST(quota_backoff_suppresses_next_capture);
    RUN_TEST(quota_backoff_honours_retry_after_sec);

    /* Group D: PSRAM-absent / no-op */
    RUN_TEST(psram_absent_init_returns_error);
    RUN_TEST(register_without_psram_does_not_crash);

    /* Group E: MCP tool */
    RUN_TEST(mcp_capture_returns_queued_on_success);
    RUN_TEST(mcp_capture_publishes_request);

    /* Group F: HW frame-size mapping */
    RUN_TEST(hw_init_maps_svga_to_real_framesize);
    RUN_TEST(hw_init_maps_qvga_to_real_framesize);
    RUN_TEST(hw_init_maps_uxga_to_real_framesize);

    /* Group G: AE/AWB settle frames */
    RUN_TEST(settle_discards_configured_frame_count);
    RUN_TEST(settle_zero_frames_discards_nothing);
    RUN_TEST(settle_frames_are_not_uploaded);
    RUN_TEST(upload_uses_frame_after_settle);
    RUN_TEST(settle_survives_null_frame);

    printf("\n═══════════════════════════════════════════\n");
    printf("  Results: %d passed, %d failed\n", s_passed, s_failed);
    printf("═══════════════════════════════════════════\n");

    return s_failed > 0 ? 1 : 0;
}
