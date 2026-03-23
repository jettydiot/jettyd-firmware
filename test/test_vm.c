/**
 * @file test_vm.c
 * @brief Unit tests for the JettyScript rule VM.
 *
 * Tests run on the host (Linux target) with mock drivers.
 * Build: idf.py --target linux build
 * Run:   ./build/test_vm
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

/* ───────────────────────────── Mock ESP-IDF ───────────────────────────────── */

/* Minimal mocks for host-side testing. In a real test harness these
 * would be provided by the ESP-IDF Linux target or a stub library. */

typedef int esp_err_t;
#define ESP_OK              0
#define ESP_FAIL            (-1)
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_NO_MEM      0x101
#define ESP_ERR_NOT_FOUND   0x105
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_INVALID_STATE 0x103

static const char *esp_err_to_name(esp_err_t err) { return "mock"; }
static uint32_t esp_get_free_heap_size(void) { return 128000; }

/* Mock time */
static int64_t s_mock_time_us = 0;
static int64_t esp_timer_get_time(void) { return s_mock_time_us; }
static void advance_time_us(int64_t us) { s_mock_time_us += us; }
static void advance_time_sec(int sec) { s_mock_time_us += (int64_t)sec * 1000000LL; }

/* Mock FreeRTOS */
#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY 0xFFFFFFFF
typedef void *TaskHandle_t;
typedef int BaseType_t;
#define pdPASS 1
#define pdFALSE 0
#define pdTRUE 1
static void vTaskDelay(uint32_t ticks) { (void)ticks; }
static BaseType_t xTaskCreate(void *fn, const char *name, uint32_t stack,
                               void *param, int prio, TaskHandle_t *handle) {
    return pdPASS;
}
static void vTaskDelete(TaskHandle_t t) {}

/* Mock NVS */
static esp_err_t jettyd_nvs_write_blob(const char *ns, const char *key,
                                        const void *data, size_t len) { return ESP_OK; }
static esp_err_t jettyd_nvs_read_blob(const char *ns, const char *key,
                                       void *buf, size_t *len) { return ESP_ERR_NVS_NOT_FOUND; }

/* Mock MQTT */
static char s_last_pub_topic[128] = {0};
static char s_last_pub_data[2048] = {0};
static esp_err_t jettyd_mqtt_publish(const char *topic, const char *data,
                                      uint8_t qos, bool retain) {
    strncpy(s_last_pub_topic, topic, sizeof(s_last_pub_topic) - 1);
    strncpy(s_last_pub_data, data, sizeof(s_last_pub_data) - 1);
    return ESP_OK;
}
static esp_err_t jettyd_mqtt_build_topic(char *buf, size_t len, const char *suffix) {
    snprintf(buf, len, "jettyd/test_tenant/test_device/%s", suffix);
    return ESP_OK;
}

/* Mock telemetry */
static int s_telemetry_publish_count = 0;
static esp_err_t jettyd_telemetry_publish(const char **metrics, uint8_t count) {
    s_telemetry_publish_count++;
    return ESP_OK;
}
static esp_err_t jettyd_telemetry_publish_all(void) {
    s_telemetry_publish_count++;
    return ESP_OK;
}
static esp_err_t jettyd_telemetry_publish_alert(const char *msg, const char *sev) {
    return ESP_OK;
}

/* Mock shadow */
static esp_err_t jettyd_shadow_update(const char *name, void *val) { return ESP_OK; }

/* ESP logging stubs */
#define ESP_LOGI(tag, fmt, ...) printf("[I] " fmt "\n", ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("[W] " fmt "\n", ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("[E] " fmt "\n", ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) /* silent */

/* ───────────────────────────── Mock Driver ────────────────────────────────── */

#include "jettyd_driver.h"

static float s_mock_soil_moisture = 50.0f;
static bool s_mock_valve_state = false;
static int s_mock_switch_on_count = 0;
static int s_mock_switch_off_count = 0;
static uint32_t s_mock_last_switch_on_duration = 0;

static jettyd_value_t mock_soil_read(const char *cap) {
    jettyd_value_t val = {.type = JETTYD_VAL_FLOAT, .float_val = s_mock_soil_moisture, .valid = true};
    return val;
}

static esp_err_t mock_valve_switch_on(uint32_t duration_ms) {
    s_mock_valve_state = true;
    s_mock_switch_on_count++;
    s_mock_last_switch_on_duration = duration_ms;
    return ESP_OK;
}

static esp_err_t mock_valve_switch_off(void) {
    s_mock_valve_state = false;
    s_mock_switch_off_count++;
    return ESP_OK;
}

static bool mock_valve_get_state(void) {
    return s_mock_valve_state;
}

static jettyd_value_t mock_valve_read(const char *cap) {
    jettyd_value_t val = {.type = JETTYD_VAL_BOOL, .bool_val = s_mock_valve_state, .valid = true};
    return val;
}

static void register_mock_drivers(void) {
    jettyd_driver_registry_init();

    /* Mock soil moisture sensor */
    static jettyd_driver_t soil_drv = {0};
    strncpy(soil_drv.instance, "soil", JETTYD_MAX_INSTANCE_NAME);
    strncpy(soil_drv.driver_name, "soil_moisture", 32);
    soil_drv.capability_count = 1;
    strncpy(soil_drv.capabilities[0].name, "moisture", 32);
    soil_drv.capabilities[0].type = JETTYD_CAP_READABLE;
    soil_drv.capabilities[0].value_type = JETTYD_VAL_FLOAT;
    soil_drv.capabilities[0].min_value = 0;
    soil_drv.capabilities[0].max_value = 100;
    soil_drv.read = mock_soil_read;
    jettyd_driver_registry_add(&soil_drv);

    /* Mock valve relay */
    static jettyd_driver_t valve_drv = {0};
    strncpy(valve_drv.instance, "valve", JETTYD_MAX_INSTANCE_NAME);
    strncpy(valve_drv.driver_name, "relay", 32);
    valve_drv.capability_count = 1;
    strncpy(valve_drv.capabilities[0].name, "state", 32);
    valve_drv.capabilities[0].type = JETTYD_CAP_SWITCHABLE;
    valve_drv.capabilities[0].value_type = JETTYD_VAL_BOOL;
    valve_drv.read = mock_valve_read;
    valve_drv.switch_on = mock_valve_switch_on;
    valve_drv.switch_off = mock_valve_switch_off;
    valve_drv.get_state = mock_valve_get_state;
    jettyd_driver_registry_add(&valve_drv);
}

/* ───────────────────────────── Include VM (source) ────────────────────────── */

/* For host-side testing, we include the VM source directly with mocks.
 * In production, proper ESP-IDF component linking is used. */

/* Provide cJSON stubs or include the real cJSON source for testing.
 * Here we assume cJSON is available as a header-only test dependency. */
#include "cJSON.h"

/* The VM source references these — we've mocked them above */
#define JETTYD_NVS_NS_VM "jettyd_vm"
static const char *JETTYD_FIRMWARE_VERSION = "1.0.0-test";
static const char *JETTYD_DEVICE_TYPE = "test-device";

typedef struct {
    uint32_t heartbeat_interval_sec;
} jettyd_config_t;
static jettyd_config_t s_test_config = {.heartbeat_interval_sec = 60};
static const jettyd_config_t *jettyd_get_config(void) { return &s_test_config; }

/* Include VM header for types */
#include "jettyd_vm.h"

/* ───────────────────────────── Tests ──────────────────────────────────────── */

static int s_tests_passed = 0;
static int s_tests_failed = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("\n--- Test: %s ---\n", #name); \
    reset_test_state(); \
    test_##name(); \
    printf("--- %s: PASSED ---\n", #name); \
    s_tests_passed++; \
} while(0)

#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        s_tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))
#define ASSERT_NEQ(a, b) ASSERT_TRUE((a) != (b))

static void reset_test_state(void) {
    s_mock_time_us = 0;
    s_mock_soil_moisture = 50.0f;
    s_mock_valve_state = false;
    s_mock_switch_on_count = 0;
    s_mock_switch_off_count = 0;
    s_telemetry_publish_count = 0;
    memset(s_last_pub_topic, 0, sizeof(s_last_pub_topic));
    memset(s_last_pub_data, 0, sizeof(s_last_pub_data));
    register_mock_drivers();
    jettyd_vm_init();
}

/* ── Test: Load valid config ── */
TEST(load_valid_config) {
    const char *config = "{"
        "\"version\": 1,"
        "\"rules\": [{"
            "\"id\": \"r1\","
            "\"enabled\": true,"
            "\"when\": {"
                "\"type\": \"threshold\","
                "\"sensor\": \"soil.moisture\","
                "\"op\": \"<\","
                "\"value\": 30,"
                "\"debounce\": 10"
            "},"
            "\"then\": [{"
                "\"action\": \"switch_on\","
                "\"target\": \"valve\","
                "\"params\": {\"duration\": 300}"
            "}]"
        "}],"
        "\"heartbeats\": [{"
            "\"id\": \"h1\","
            "\"every\": 60,"
            "\"metrics\": [\"soil.moisture\", \"valve.state\"]"
        "}]"
    "}";

    jettyd_vm_error_t errors[16];
    uint8_t error_count = 0;
    esp_err_t err = jettyd_vm_load_config(config, strlen(config), errors, &error_count);

    ASSERT_EQ(err, ESP_OK);
    ASSERT_EQ(error_count, 0);

    const jettyd_vm_state_t *state = jettyd_vm_get_state();
    ASSERT_EQ(state->rule_count, 1);
    ASSERT_EQ(state->heartbeat_count, 1);
    ASSERT_TRUE(state->loaded);
    ASSERT_TRUE(strcmp(state->rules[0].id, "r1") == 0);
    ASSERT_EQ(state->heartbeats[0].interval_sec, 60);
}

/* ── Test: Reject config with non-existent driver ── */
TEST(reject_unknown_sensor) {
    const char *config = "{"
        "\"version\": 1,"
        "\"rules\": [{"
            "\"id\": \"r1\","
            "\"when\": {"
                "\"type\": \"threshold\","
                "\"sensor\": \"nonexistent.value\","
                "\"op\": \"<\","
                "\"value\": 30"
            "},"
            "\"then\": [{\"action\": \"switch_on\", \"target\": \"valve\"}]"
        "}],"
        "\"heartbeats\": []"
    "}";

    jettyd_vm_error_t errors[16];
    uint8_t error_count = 0;
    esp_err_t err = jettyd_vm_load_config(config, strlen(config), errors, &error_count);

    ASSERT_EQ(err, ESP_ERR_INVALID_ARG);
    ASSERT_TRUE(error_count > 0);
    printf("  Rejection error: %s\n", errors[0].error);
}

/* ── Test: Reject bad version ── */
TEST(reject_bad_version) {
    const char *config = "{\"version\": 99, \"rules\": [], \"heartbeats\": []}";

    jettyd_vm_error_t errors[16];
    uint8_t error_count = 0;
    esp_err_t err = jettyd_vm_load_config(config, strlen(config), errors, &error_count);

    ASSERT_EQ(err, ESP_ERR_INVALID_ARG);
}

/* ── Test: Reject too many rules ── */
TEST(reject_too_many_rules) {
    /* Build config with 17 rules (max is 16) */
    char config[8192];
    int pos = snprintf(config, sizeof(config), "{\"version\":1,\"rules\":[");
    for (int i = 0; i < 17; i++) {
        if (i > 0) config[pos++] = ',';
        pos += snprintf(config + pos, sizeof(config) - pos,
            "{\"id\":\"r%d\",\"when\":{\"type\":\"threshold\","
            "\"sensor\":\"soil.moisture\",\"op\":\"<\",\"value\":30},"
            "\"then\":[{\"action\":\"switch_on\",\"target\":\"valve\"}]}", i);
    }
    pos += snprintf(config + pos, sizeof(config) - pos, "],\"heartbeats\":[]}");

    jettyd_vm_error_t errors[16];
    uint8_t error_count = 0;
    esp_err_t err = jettyd_vm_load_config(config, pos, errors, &error_count);

    ASSERT_EQ(err, ESP_ERR_INVALID_ARG);
}

/* ── Test: Edge detection — condition fires only on crossing ── */
TEST(edge_detection) {
    const char *config = "{"
        "\"version\": 1,"
        "\"rules\": [{"
            "\"id\": \"r1\","
            "\"when\": {\"type\": \"threshold\", \"sensor\": \"soil.moisture\","
            "           \"op\": \"<\", \"value\": 30, \"debounce\": 0},"
            "\"then\": [{\"action\": \"switch_on\", \"target\": \"valve\","
            "            \"params\": {\"duration\": 60}}]"
        "}],"
        "\"heartbeats\": []"
    "}";

    jettyd_vm_error_t errors[16];
    uint8_t error_count = 0;
    jettyd_vm_load_config(config, strlen(config), errors, &error_count);

    /* Moisture at 50% — above threshold, no trigger */
    s_mock_soil_moisture = 50.0f;
    jettyd_vm_tick();
    ASSERT_EQ(s_mock_switch_on_count, 0);

    /* Drop below threshold — should trigger */
    s_mock_soil_moisture = 25.0f;
    advance_time_sec(1);
    jettyd_vm_tick();
    ASSERT_EQ(s_mock_switch_on_count, 1);

    /* Still below — should NOT trigger again (edge detection) */
    s_mock_soil_moisture = 20.0f;
    advance_time_sec(1);
    jettyd_vm_tick();
    ASSERT_EQ(s_mock_switch_on_count, 1);

    /* Rise above, then drop again — should trigger */
    s_mock_soil_moisture = 50.0f;
    advance_time_sec(1);
    jettyd_vm_tick();
    s_mock_soil_moisture = 25.0f;
    advance_time_sec(1);
    jettyd_vm_tick();
    ASSERT_EQ(s_mock_switch_on_count, 2);
}

/* ── Test: Debounce — no re-trigger within window ── */
TEST(debounce) {
    const char *config = "{"
        "\"version\": 1,"
        "\"rules\": [{"
            "\"id\": \"r1\","
            "\"when\": {\"type\": \"threshold\", \"sensor\": \"soil.moisture\","
            "           \"op\": \"<\", \"value\": 30, \"debounce\": 300},"
            "\"then\": [{\"action\": \"switch_on\", \"target\": \"valve\"}]"
        "}],"
        "\"heartbeats\": []"
    "}";

    jettyd_vm_error_t errors[16];
    uint8_t error_count = 0;
    jettyd_vm_load_config(config, strlen(config), errors, &error_count);

    /* First trigger */
    s_mock_soil_moisture = 25.0f;
    jettyd_vm_tick();
    ASSERT_EQ(s_mock_switch_on_count, 1);

    /* Reset condition and try again within debounce window */
    s_mock_soil_moisture = 50.0f;
    advance_time_sec(10);
    jettyd_vm_tick();
    s_mock_soil_moisture = 25.0f;
    advance_time_sec(1);
    jettyd_vm_tick();
    ASSERT_EQ(s_mock_switch_on_count, 1); /* Still 1 — debounced */

    /* Wait past debounce window and try again */
    s_mock_soil_moisture = 50.0f;
    advance_time_sec(300);
    jettyd_vm_tick();
    s_mock_soil_moisture = 25.0f;
    advance_time_sec(1);
    jettyd_vm_tick();
    ASSERT_EQ(s_mock_switch_on_count, 2); /* Now fires */
}

/* ── Test: Heartbeat fires at correct interval ── */
TEST(heartbeat_interval) {
    const char *config = "{"
        "\"version\": 1,"
        "\"rules\": [],"
        "\"heartbeats\": [{"
            "\"id\": \"h1\","
            "\"every\": 60,"
            "\"metrics\": [\"soil.moisture\"]"
        "}]"
    "}";

    jettyd_vm_error_t errors[16];
    uint8_t error_count = 0;
    jettyd_vm_load_config(config, strlen(config), errors, &error_count);

    /* First tick should fire (time since last = infinity) */
    jettyd_vm_tick();
    ASSERT_EQ(s_telemetry_publish_count, 1);

    /* 30 seconds later — should not fire */
    advance_time_sec(30);
    jettyd_vm_tick();
    ASSERT_EQ(s_telemetry_publish_count, 1);

    /* 60 seconds total — should fire */
    advance_time_sec(30);
    jettyd_vm_tick();
    ASSERT_EQ(s_telemetry_publish_count, 2);
}

/* ── Test: Config replacement ── */
TEST(config_replacement) {
    /* Load config A with 2 rules */
    const char *config_a = "{"
        "\"version\": 1,"
        "\"rules\": ["
            "{\"id\":\"r1\",\"when\":{\"type\":\"threshold\",\"sensor\":\"soil.moisture\","
            " \"op\":\"<\",\"value\":30},\"then\":[{\"action\":\"switch_on\",\"target\":\"valve\"}]},"
            "{\"id\":\"r2\",\"when\":{\"type\":\"threshold\",\"sensor\":\"soil.moisture\","
            " \"op\":\">\",\"value\":80},\"then\":[{\"action\":\"switch_off\",\"target\":\"valve\"}]}"
        "],"
        "\"heartbeats\": [{\"id\":\"h1\",\"every\":60,\"metrics\":[\"soil.moisture\"]}]"
    "}";

    jettyd_vm_error_t errors[16];
    uint8_t error_count = 0;
    jettyd_vm_load_config(config_a, strlen(config_a), errors, &error_count);

    const jettyd_vm_state_t *state = jettyd_vm_get_state();
    ASSERT_EQ(state->rule_count, 2);

    /* Load config B with 1 rule — should replace entirely */
    const char *config_b = "{"
        "\"version\": 1,"
        "\"rules\": ["
            "{\"id\":\"r3\",\"when\":{\"type\":\"threshold\",\"sensor\":\"soil.moisture\","
            " \"op\":\"<\",\"value\":20},\"then\":[{\"action\":\"switch_on\",\"target\":\"valve\"}]}"
        "],"
        "\"heartbeats\": [{\"id\":\"h2\",\"every\":120,\"metrics\":[\"soil.moisture\"]}]"
    "}";

    jettyd_vm_load_config(config_b, strlen(config_b), errors, &error_count);

    state = jettyd_vm_get_state();
    ASSERT_EQ(state->rule_count, 1);
    ASSERT_TRUE(strcmp(state->rules[0].id, "r3") == 0);
    ASSERT_EQ(state->heartbeats[0].interval_sec, 120);
}

/* ── Test: Template substitution ── */
TEST(template_substitution) {
    char out[256];
    s_mock_soil_moisture = 42.3f;

    jettyd_vm_template_subst("Moisture at {{soil.moisture}}%", out, sizeof(out));
    printf("  Template result: %s\n", out);
    ASSERT_TRUE(strstr(out, "42.3") != NULL);
    ASSERT_TRUE(strstr(out, "Moisture at") != NULL);
}

/* ── Test: Heartbeat interval bounds ── */
TEST(heartbeat_bounds) {
    /* Below minimum should be clamped */
    const char *config = "{"
        "\"version\": 1,"
        "\"rules\": [],"
        "\"heartbeats\": [{\"id\":\"h1\",\"every\":5,\"metrics\":[]}]"
    "}";

    jettyd_vm_error_t errors[16];
    uint8_t error_count = 0;
    jettyd_vm_load_config(config, strlen(config), errors, &error_count);

    /* Should have an error about interval being below minimum */
    ASSERT_TRUE(error_count > 0 || jettyd_vm_get_state()->heartbeats[0].interval_sec >= 10);
}

/* ── Test: Reject invalid JSON ── */
TEST(reject_invalid_json) {
    const char *config = "this is not json";

    jettyd_vm_error_t errors[16];
    uint8_t error_count = 0;
    esp_err_t err = jettyd_vm_load_config(config, strlen(config), errors, &error_count);

    ASSERT_EQ(err, ESP_ERR_INVALID_ARG);
}

/* ── Test: Config size limit ── */
TEST(config_size_limit) {
    char big[JETTYD_VM_CONFIG_MAX_SIZE + 100];
    memset(big, ' ', sizeof(big));
    big[0] = '{';
    big[sizeof(big) - 2] = '}';
    big[sizeof(big) - 1] = '\0';

    jettyd_vm_error_t errors[16];
    uint8_t error_count = 0;
    esp_err_t err = jettyd_vm_load_config(big, sizeof(big) - 1, errors, &error_count);

    ASSERT_EQ(err, ESP_ERR_INVALID_ARG);
}

/* ───────────────────────────── Main ───────────────────────────────────────── */

int main(void)
{
    printf("═══════════════════════════════════════\n");
    printf("  Jettyd VM Unit Tests\n");
    printf("═══════════════════════════════════════\n");

    RUN_TEST(load_valid_config);
    RUN_TEST(reject_unknown_sensor);
    RUN_TEST(reject_bad_version);
    RUN_TEST(reject_too_many_rules);
    RUN_TEST(edge_detection);
    RUN_TEST(debounce);
    RUN_TEST(heartbeat_interval);
    RUN_TEST(config_replacement);
    RUN_TEST(template_substitution);
    RUN_TEST(heartbeat_bounds);
    RUN_TEST(reject_invalid_json);
    RUN_TEST(config_size_limit);

    printf("\n═══════════════════════════════════════\n");
    printf("  Results: %d passed, %d failed\n", s_tests_passed, s_tests_failed);
    printf("═══════════════════════════════════════\n");

    return s_tests_failed > 0 ? 1 : 0;
}
