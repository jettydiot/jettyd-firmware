/**
 * @file test_manifest.c
 * @brief Unit tests for jettyd_publish_manifest() (FLU-113).
 *
 * Tests the JSON-building, driver-name sanitization, and publish-topic
 * logic without real hardware or a live MQTT broker.
 */

/* ESP-IDF stubs for host testing */
#include "mocks/esp_idf_stubs.h"
#include "jettyd_driver.h"

/* ── Minimal stubs for modules manifest.c depends on ───────────────────────── */

/* jettyd_provision: pretend device is provisioned */
#include "jettyd_provision.h"

static jettyd_provision_state_t s_prov_state = {
    .tenant_id   = "tenant_test",
    .device_id   = "device_test",
    .device_key  = "dk_test_key",
    .fleet_token = "",
    .mqtt_uri    = "mqtt://localhost:1883",
    .provisioned = true,
};

const jettyd_provision_state_t *jettyd_provision_get_state(void)
{
    return &s_prov_state;
}

esp_err_t jettyd_provision_init(void)  { return ESP_OK; }
bool      jettyd_provision_is_provisioned(void) { return true; }
esp_err_t jettyd_provision_run(void)   { return ESP_OK; }
esp_err_t jettyd_provision_store(const jettyd_provision_state_t *s) { (void)s; return ESP_OK; }
esp_err_t jettyd_provision_clear(void) { return ESP_OK; }

/* jettyd global version string (normally defined in jettyd.c) */
const char *JETTYD_FIRMWARE_VERSION = "1.2.3";

/* jettyd_mqtt stubs — capture what was published */
#include "jettyd_mqtt.h"

static bool  s_mqtt_connected       = true;
static char  s_published_topic[JETTYD_MQTT_MAX_TOPIC]   = {0};
static char  s_published_payload[JETTYD_MQTT_MAX_PAYLOAD] = {0};
static uint8_t s_published_qos      = 0;
static bool  s_published_retain     = false;
static bool  s_publish_called       = false;

bool jettyd_mqtt_is_connected(void) { return s_mqtt_connected; }

esp_err_t jettyd_mqtt_build_topic(char *buf, size_t buf_len, const char *suffix)
{
    int w = snprintf(buf, buf_len, "jettyd/%s/%s/%s",
                     s_prov_state.tenant_id, s_prov_state.device_key, suffix);
    if (w < 0 || (size_t)w >= buf_len) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

esp_err_t jettyd_mqtt_publish(const char *topic, const char *data,
                               uint8_t qos, bool retain)
{
    s_publish_called = true;
    strncpy(s_published_topic,   topic, sizeof(s_published_topic)   - 1);
    strncpy(s_published_payload, data,  sizeof(s_published_payload) - 1);
    s_published_qos    = qos;
    s_published_retain = retain;
    return ESP_OK;
}

/* Stub out the rest of the MQTT API so the linker is happy */
esp_err_t jettyd_mqtt_init(const jettyd_mqtt_config_t *c)       { (void)c; return ESP_OK; }
esp_err_t jettyd_mqtt_connect(void)                              { return ESP_OK; }
esp_err_t jettyd_mqtt_disconnect(void)                           { return ESP_OK; }
esp_err_t jettyd_mqtt_subscribe(const char *t, uint8_t q,
                                 jettyd_mqtt_msg_cb_t cb)        { (void)t;(void)q;(void)cb; return ESP_OK; }
esp_err_t jettyd_mqtt_reconfigure(const char *u, const char *p) { (void)u;(void)p; return ESP_OK; }
esp_err_t jettyd_mqtt_flush_buffer(void)                         { return ESP_OK; }

/* Now include the unit under test */
#include "../jettyd/src/manifest.c"

/* ───────────────────────────── Test Framework ─────────────────────────────── */

static int s_passed = 0;
static int s_failed = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("\n--- Test: %s ---\n", #name); \
    jettyd_driver_registry_init(); \
    s_publish_called   = false; \
    s_mqtt_connected   = true;  \
    s_prov_state.provisioned = true; \
    memset(s_published_topic,   0, sizeof(s_published_topic));   \
    memset(s_published_payload, 0, sizeof(s_published_payload)); \
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

#define ASSERT_EQ(a, b)       ASSERT_TRUE((a) == (b))
#define ASSERT_STR_EQ(a, b)   ASSERT_TRUE(strcmp((a), (b)) == 0)
#define ASSERT_STR_CONTAINS(haystack, needle) \
    ASSERT_TRUE(strstr((haystack), (needle)) != NULL)
#define ASSERT_STR_NOT_CONTAINS(haystack, needle) \
    ASSERT_TRUE(strstr((haystack), (needle)) == NULL)

/* ───────────────────────────── Tests ──────────────────────────────────────── */

TEST(publishes_to_correct_topic) {
    esp_err_t err = jettyd_publish_manifest();
    ASSERT_EQ(err, ESP_OK);
    ASSERT_TRUE(s_publish_called);
    ASSERT_STR_EQ(s_published_topic,
                  "jettyd/tenant_test/dk_test_key/manifest");
}

TEST(qos_1_retained) {
    jettyd_publish_manifest();
    ASSERT_TRUE(s_publish_called);
    ASSERT_EQ(s_published_qos, 1);
    ASSERT_TRUE(s_published_retain);
}

TEST(payload_contains_firmware_version) {
    jettyd_publish_manifest();
    ASSERT_STR_CONTAINS(s_published_payload, "\"firmware\":\"1.2.3\"");
}

TEST(payload_contains_chip_field) {
    jettyd_publish_manifest();
    ASSERT_STR_CONTAINS(s_published_payload, "\"chip\":");
}

TEST(empty_drivers_array_when_none_registered) {
    /* No drivers registered */
    jettyd_publish_manifest();
    ASSERT_STR_CONTAINS(s_published_payload, "\"drivers\":[]");
}

TEST(registered_drivers_appear_in_payload) {
    jettyd_driver_t drv = {0};
    strncpy(drv.instance,    "soil",          JETTYD_MAX_INSTANCE_NAME - 1);
    strncpy(drv.driver_name, "soil_moisture", sizeof(drv.driver_name)  - 1);
    jettyd_driver_registry_add(&drv);

    jettyd_publish_manifest();
    ASSERT_STR_CONTAINS(s_published_payload, "\"soil\"");
}

TEST(multiple_drivers_all_appear) {
    jettyd_driver_t d1 = {0}, d2 = {0};
    strncpy(d1.instance, "valve", JETTYD_MAX_INSTANCE_NAME - 1);
    strncpy(d2.instance, "led",   JETTYD_MAX_INSTANCE_NAME - 1);
    jettyd_driver_registry_add(&d1);
    jettyd_driver_registry_add(&d2);

    jettyd_publish_manifest();
    ASSERT_STR_CONTAINS(s_published_payload, "\"valve\"");
    ASSERT_STR_CONTAINS(s_published_payload, "\"led\"");
}

TEST(unsafe_driver_name_is_skipped) {
    jettyd_driver_t drv = {0};
    /* Instance name contains a hyphen — must be rejected */
    strncpy(drv.instance, "bad-name", JETTYD_MAX_INSTANCE_NAME - 1);
    jettyd_driver_registry_add(&drv);

    jettyd_publish_manifest();
    ASSERT_STR_NOT_CONTAINS(s_published_payload, "\"bad-name\"");
}

TEST(safe_driver_name_with_underscore_is_included) {
    jettyd_driver_t drv = {0};
    strncpy(drv.instance, "soil_2", JETTYD_MAX_INSTANCE_NAME - 1);
    jettyd_driver_registry_add(&drv);

    jettyd_publish_manifest();
    ASSERT_STR_CONTAINS(s_published_payload, "\"soil_2\"");
}

TEST(skipped_when_not_provisioned) {
    s_prov_state.provisioned = false;
    esp_err_t err = jettyd_publish_manifest();
    ASSERT_EQ(err, ESP_OK);
    ASSERT_TRUE(!s_publish_called);
}

TEST(skipped_when_not_connected) {
    s_mqtt_connected = false;
    esp_err_t err = jettyd_publish_manifest();
    ASSERT_EQ(err, ESP_OK);
    ASSERT_TRUE(!s_publish_called);
}

/* ───────────────────────────── Main ───────────────────────────────────────── */

int main(void)
{
    printf("═══════════════════════════════════════\n");
    printf("  Jettyd Manifest Unit Tests (FLU-113)\n");
    printf("═══════════════════════════════════════\n");

    RUN_TEST(publishes_to_correct_topic);
    RUN_TEST(qos_1_retained);
    RUN_TEST(payload_contains_firmware_version);
    RUN_TEST(payload_contains_chip_field);
    RUN_TEST(empty_drivers_array_when_none_registered);
    RUN_TEST(registered_drivers_appear_in_payload);
    RUN_TEST(multiple_drivers_all_appear);
    RUN_TEST(unsafe_driver_name_is_skipped);
    RUN_TEST(safe_driver_name_with_underscore_is_included);
    RUN_TEST(skipped_when_not_provisioned);
    RUN_TEST(skipped_when_not_connected);

    printf("\n═══════════════════════════════════════\n");
    printf("  Results: %d passed, %d failed\n", s_passed, s_failed);
    printf("═══════════════════════════════════════\n");

    return s_failed > 0 ? 1 : 0;
}
