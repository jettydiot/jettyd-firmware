/**
 * @file test_mcp_tools.c
 * @brief Unit tests for MCP tool registration, serialization, and dispatch (FLU-137).
 */

#include "mocks/esp_idf_stubs.h"
#include "jettyd_driver.h"
#include "jettyd_mcp.h"

/* ── Provision stub ──────────────────────────────────────────────────────── */

#include "jettyd_provision.h"

static jettyd_provision_state_t s_prov_state = {
    .tenant_id   = "tenant_test",
    .device_id   = "device_test",
    .device_key  = "dk_test_key",
    .fleet_token = "",
    .mqtt_uri    = "mqtt://localhost:1883",
    .provisioned = true,
};

const jettyd_provision_state_t *jettyd_provision_get_state(void) { return &s_prov_state; }
esp_err_t jettyd_provision_init(void)  { return ESP_OK; }
bool      jettyd_provision_is_provisioned(void) { return true; }
esp_err_t jettyd_provision_run(void)   { return ESP_OK; }
esp_err_t jettyd_provision_store(const jettyd_provision_state_t *s) { (void)s; return ESP_OK; }
esp_err_t jettyd_provision_clear(void) { return ESP_OK; }

/* ── MQTT stubs ──────────────────────────────────────────────────────────── */

#include "jettyd_mqtt.h"

static bool   s_mqtt_connected                      = true;
static char   s_published_topic[JETTYD_MQTT_MAX_TOPIC]   = {0};
static char   s_published_payload[4096]             = {0};
static uint8_t s_published_qos                      = 0;
static bool   s_published_retain                    = false;
static bool   s_publish_called                      = false;
static char   s_subscribed_topic[JETTYD_MQTT_MAX_TOPIC]  = {0};
static bool   s_subscribe_called                    = false;

bool jettyd_mqtt_is_connected(void) { return s_mqtt_connected; }

esp_err_t jettyd_mqtt_build_topic(char *buf, size_t buf_len, const char *suffix)
{
    int w = snprintf(buf, buf_len, "jettyd/%s/%s/%s",
                     s_prov_state.tenant_id, s_prov_state.device_key, suffix);
    if (w < 0 || (size_t)w >= buf_len) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

esp_err_t jettyd_mqtt_publish(const char *topic, const char *data, uint8_t qos, bool retain)
{
    s_publish_called = true;
    strncpy(s_published_topic,   topic, sizeof(s_published_topic) - 1);
    strncpy(s_published_payload, data,  sizeof(s_published_payload) - 1);
    s_published_qos    = qos;
    s_published_retain = retain;
    return ESP_OK;
}

esp_err_t jettyd_mqtt_subscribe(const char *topic, uint8_t qos, jettyd_mqtt_msg_cb_t cb)
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

/* ── Include unit under test ─────────────────────────────────────────────── */

#include "../jettyd/src/mcp.c"

/* ── Test framework ──────────────────────────────────────────────────────── */

static int s_passed = 0;
static int s_failed = 0;

#define TEST(name) static void test_##name(void)

#define RUN_TEST(name) do { \
    printf("\n--- Test: %s ---\n", #name); \
    jettyd_driver_registry_init(); \
    s_publish_called   = false; \
    s_subscribe_called = false; \
    s_mqtt_connected   = true;  \
    s_prov_state.provisioned = true; \
    memset(s_published_topic,   0, sizeof(s_published_topic));   \
    memset(s_published_payload, 0, sizeof(s_published_payload)); \
    memset(s_subscribed_topic,  0, sizeof(s_subscribed_topic));  \
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

#define ASSERT_EQ(a, b)     ASSERT_TRUE((a) == (b))
#define ASSERT_STR_EQ(a, b) ASSERT_TRUE(strcmp((a), (b)) == 0)
#define ASSERT_STR_CONTAINS(hay, needle) ASSERT_TRUE(strstr((hay), (needle)) != NULL)
#define ASSERT_STR_NOT_CONTAINS(hay, needle) ASSERT_TRUE(strstr((hay), (needle)) == NULL)

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static bool s_handler_called = false;
static char s_handler_params[256] = {0};

static esp_err_t mock_ok_handler(const char *params, char *out, size_t out_len)
{
    s_handler_called = true;
    if (params) strncpy(s_handler_params, params, sizeof(s_handler_params) - 1);
    snprintf(out, out_len, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t mock_fail_handler(const char *params, char *out, size_t out_len)
{
    (void)params; (void)out; (void)out_len;
    return ESP_FAIL;
}

static jettyd_driver_t make_driver_with_tool(const char *instance,
                                              const char *tool_name,
                                              const char *desc,
                                              const char *schema)
{
    jettyd_driver_t drv = {0};
    strncpy(drv.instance, instance, JETTYD_MAX_INSTANCE_NAME - 1);
    strncpy(drv.driver_name, instance, sizeof(drv.driver_name) - 1);
    strncpy(drv.mcp_tools[0].name, tool_name, sizeof(drv.mcp_tools[0].name) - 1);
    drv.mcp_tools[0].description = desc;
    drv.mcp_tools[0].input_schema_json = schema;
    drv.mcp_tools[0].handler = mock_ok_handler;
    drv.mcp_tool_count = 1;
    return drv;
}

/* ── Group A: driver struct MCP fields ───────────────────────────────────── */

TEST(driver_mcp_tool_count_preserved) {
    jettyd_driver_t drv = make_driver_with_tool("relay", "relay_turn_on",
                                                "Turn the relay on",
                                                "{\"type\":\"object\",\"properties\":{}}");
    jettyd_driver_registry_add(&drv);
    const jettyd_driver_t *got = jettyd_driver_find("relay");
    ASSERT_TRUE(got != NULL);
    ASSERT_EQ(got->mcp_tool_count, 1);
}

TEST(driver_mcp_tool_name_preserved) {
    jettyd_driver_t drv = make_driver_with_tool("led", "led_turn_on", "Turn on", NULL);
    jettyd_driver_registry_add(&drv);
    const jettyd_driver_t *got = jettyd_driver_find("led");
    ASSERT_TRUE(got != NULL);
    ASSERT_STR_EQ(got->mcp_tools[0].name, "led_turn_on");
}

TEST(driver_without_mcp_tools_has_zero_count) {
    jettyd_driver_t drv = {0};
    strncpy(drv.instance, "bare", JETTYD_MAX_INSTANCE_NAME - 1);
    jettyd_driver_registry_add(&drv);
    const jettyd_driver_t *got = jettyd_driver_find("bare");
    ASSERT_TRUE(got != NULL);
    ASSERT_EQ(got->mcp_tool_count, 0);
}

TEST(driver_mcp_handler_pointer_preserved) {
    jettyd_driver_t drv = make_driver_with_tool("sensor", "sensor_read", "Read", NULL);
    drv.mcp_tools[0].handler = mock_ok_handler;
    jettyd_driver_registry_add(&drv);
    const jettyd_driver_t *got = jettyd_driver_find("sensor");
    ASSERT_TRUE(got != NULL);
    ASSERT_TRUE(got->mcp_tools[0].handler == mock_ok_handler);
}

/* ── Group B: serializer ──────────────────────────────────────────────────── */

TEST(serialize_empty_produces_empty_array) {
    char buf[256];
    esp_err_t err = jettyd_mcp_serialize_tools_list(buf, sizeof(buf));
    ASSERT_EQ(err, ESP_OK);
    ASSERT_STR_CONTAINS(buf, "{\"tools\":[]}");
}

TEST(serialize_single_tool_name_appears) {
    jettyd_driver_t drv = make_driver_with_tool("relay", "relay_turn_on",
                                                "Turn on",
                                                "{\"type\":\"object\",\"properties\":{}}");
    jettyd_driver_registry_add(&drv);
    char buf[512];
    esp_err_t err = jettyd_mcp_serialize_tools_list(buf, sizeof(buf));
    ASSERT_EQ(err, ESP_OK);
    ASSERT_STR_CONTAINS(buf, "\"relay_turn_on\"");
}

TEST(serialize_tool_description_appears) {
    jettyd_driver_t drv = make_driver_with_tool("led", "led_blink",
                                                "Blink the LED",
                                                "{\"type\":\"object\",\"properties\":{}}");
    jettyd_driver_registry_add(&drv);
    char buf[512];
    jettyd_mcp_serialize_tools_list(buf, sizeof(buf));
    ASSERT_STR_CONTAINS(buf, "Blink the LED");
}

TEST(serialize_driver_field_matches_instance) {
    jettyd_driver_t drv = make_driver_with_tool("soil", "soil_read_moisture",
                                                "Read moisture", NULL);
    jettyd_driver_registry_add(&drv);
    char buf[512];
    jettyd_mcp_serialize_tools_list(buf, sizeof(buf));
    ASSERT_STR_CONTAINS(buf, "\"driver\":\"soil\"");
}

TEST(serialize_schema_appears_verbatim) {
    static const char *schema = "{\"type\":\"object\",\"properties\":{\"duration\":{\"type\":\"number\"}}}";
    jettyd_driver_t drv = make_driver_with_tool("relay", "relay_turn_on", "On", schema);
    jettyd_driver_registry_add(&drv);
    char buf[1024];
    jettyd_mcp_serialize_tools_list(buf, sizeof(buf));
    ASSERT_STR_CONTAINS(buf, "\"duration\"");
    ASSERT_STR_CONTAINS(buf, "\"inputSchema\":");
}

TEST(serialize_multiple_drivers_all_tools_appear) {
    jettyd_driver_t d1 = make_driver_with_tool("relay", "relay_turn_on", "On", NULL);
    jettyd_driver_t d2 = make_driver_with_tool("led",   "led_turn_off",  "Off", NULL);
    jettyd_driver_registry_add(&d1);
    jettyd_driver_registry_add(&d2);
    char buf[1024];
    jettyd_mcp_serialize_tools_list(buf, sizeof(buf));
    ASSERT_STR_CONTAINS(buf, "relay_turn_on");
    ASSERT_STR_CONTAINS(buf, "led_turn_off");
}

TEST(serialize_driver_with_no_tools_skipped) {
    jettyd_driver_t d_bare = {0};
    strncpy(d_bare.instance, "bare", JETTYD_MAX_INSTANCE_NAME - 1);
    jettyd_driver_registry_add(&d_bare);

    jettyd_driver_t d_tool = make_driver_with_tool("active", "active_go", "Go", NULL);
    jettyd_driver_registry_add(&d_tool);

    char buf[512];
    jettyd_mcp_serialize_tools_list(buf, sizeof(buf));
    ASSERT_STR_CONTAINS(buf, "active_go");
    ASSERT_STR_NOT_CONTAINS(buf, "bare");
}

TEST(serialize_buffer_too_small_returns_error) {
    jettyd_driver_t drv = make_driver_with_tool("relay", "relay_turn_on",
                                                "Turn on", NULL);
    jettyd_driver_registry_add(&drv);
    /* buf[20] passes the null/minimum guard but is far too small for a full tool entry */
    char buf[20];
    esp_err_t err = jettyd_mcp_serialize_tools_list(buf, sizeof(buf));
    ASSERT_EQ(err, ESP_ERR_NO_MEM);
}

TEST(serialize_null_buffer_returns_error) {
    esp_err_t err = jettyd_mcp_serialize_tools_list(NULL, 256);
    ASSERT_EQ(err, ESP_ERR_INVALID_ARG);
}

/* ── Group C: dispatch ───────────────────────────────────────────────────── */

TEST(dispatch_known_tool_calls_handler) {
    s_handler_called = false;
    jettyd_driver_t drv = make_driver_with_tool("relay", "relay_turn_on", "On", NULL);
    jettyd_driver_registry_add(&drv);

    const char *payload = "{\"id\":\"call_001\",\"tool\":\"relay_turn_on\",\"params\":{}}";
    jettyd_mcp_handle_call(payload, (int)strlen(payload));

    ASSERT_TRUE(s_handler_called);
}

TEST(dispatch_unknown_tool_publishes_error) {
    const char *payload = "{\"id\":\"call_002\",\"tool\":\"no_such_tool\",\"params\":{}}";
    jettyd_mcp_handle_call(payload, (int)strlen(payload));

    ASSERT_TRUE(s_publish_called);
    ASSERT_STR_CONTAINS(s_published_payload, "\"status\":\"error\"");
    ASSERT_STR_CONTAINS(s_published_payload, "unknown tool");
}

TEST(dispatch_id_preserved_in_result) {
    jettyd_driver_t drv = make_driver_with_tool("led", "led_on", "On", NULL);
    jettyd_driver_registry_add(&drv);

    const char *payload = "{\"id\":\"my_call_xyz\",\"tool\":\"led_on\",\"params\":{}}";
    jettyd_mcp_handle_call(payload, (int)strlen(payload));

    ASSERT_TRUE(s_publish_called);
    ASSERT_STR_CONTAINS(s_published_payload, "\"id\":\"my_call_xyz\"");
}

TEST(dispatch_success_publishes_ok_status) {
    jettyd_driver_t drv = make_driver_with_tool("led", "led_on", "On", NULL);
    jettyd_driver_registry_add(&drv);

    const char *payload = "{\"id\":\"c1\",\"tool\":\"led_on\",\"params\":{}}";
    jettyd_mcp_handle_call(payload, (int)strlen(payload));

    ASSERT_STR_CONTAINS(s_published_payload, "\"status\":\"ok\"");
}

TEST(dispatch_handler_failure_publishes_error) {
    jettyd_driver_t drv = {0};
    strncpy(drv.instance, "fail_drv", JETTYD_MAX_INSTANCE_NAME - 1);
    strncpy(drv.mcp_tools[0].name, "fail_action", 31);
    drv.mcp_tools[0].handler = mock_fail_handler;
    drv.mcp_tool_count = 1;
    jettyd_driver_registry_add(&drv);

    const char *payload = "{\"id\":\"c2\",\"tool\":\"fail_action\",\"params\":{}}";
    jettyd_mcp_handle_call(payload, (int)strlen(payload));

    ASSERT_STR_CONTAINS(s_published_payload, "\"status\":\"error\"");
}

TEST(dispatch_result_published_to_mcp_result_topic) {
    jettyd_driver_t drv = make_driver_with_tool("led", "led_on", "On", NULL);
    jettyd_driver_registry_add(&drv);

    const char *payload = "{\"id\":\"c3\",\"tool\":\"led_on\",\"params\":{}}";
    jettyd_mcp_handle_call(payload, (int)strlen(payload));

    ASSERT_TRUE(s_publish_called);
    ASSERT_STR_CONTAINS(s_published_topic, "mcp/result");
}

TEST(dispatch_missing_tool_field_returns_error) {
    const char *payload = "{\"id\":\"c4\",\"params\":{}}";
    jettyd_mcp_handle_call(payload, (int)strlen(payload));

    ASSERT_STR_CONTAINS(s_published_payload, "\"status\":\"error\"");
}

/* ── Group D: publish_mcp_tools ──────────────────────────────────────────── */

TEST(publish_mcp_tools_calls_mqtt_publish) {
    jettyd_publish_mcp_tools();
    ASSERT_TRUE(s_publish_called);
}

TEST(publish_mcp_tools_uses_tools_list_topic) {
    jettyd_publish_mcp_tools();
    ASSERT_STR_CONTAINS(s_published_topic, "tools/list");
}

TEST(publish_mcp_tools_is_retained) {
    jettyd_publish_mcp_tools();
    ASSERT_TRUE(s_published_retain);
}

TEST(publish_mcp_tools_skipped_when_disconnected) {
    s_mqtt_connected = false;
    jettyd_publish_mcp_tools();
    ASSERT_TRUE(!s_publish_called);
}

TEST(publish_mcp_tools_skipped_when_not_provisioned) {
    s_prov_state.provisioned = false;
    jettyd_publish_mcp_tools();
    ASSERT_TRUE(!s_publish_called);
}

/* ── Group E: jettyd_mcp_init subscription ───────────────────────────────── */

TEST(mcp_init_subscribes_to_mcp_call) {
    jettyd_mcp_init();
    ASSERT_TRUE(s_subscribe_called);
    ASSERT_STR_CONTAINS(s_subscribed_topic, "mcp/call");
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("═══════════════════════════════════════\n");
    printf("  Jettyd MCP Tool Dispatch Unit Tests (FLU-137)\n");
    printf("═══════════════════════════════════════\n");

    /* Group A: driver struct */
    RUN_TEST(driver_mcp_tool_count_preserved);
    RUN_TEST(driver_mcp_tool_name_preserved);
    RUN_TEST(driver_without_mcp_tools_has_zero_count);
    RUN_TEST(driver_mcp_handler_pointer_preserved);

    /* Group B: serializer */
    RUN_TEST(serialize_empty_produces_empty_array);
    RUN_TEST(serialize_single_tool_name_appears);
    RUN_TEST(serialize_tool_description_appears);
    RUN_TEST(serialize_driver_field_matches_instance);
    RUN_TEST(serialize_schema_appears_verbatim);
    RUN_TEST(serialize_multiple_drivers_all_tools_appear);
    RUN_TEST(serialize_driver_with_no_tools_skipped);
    RUN_TEST(serialize_buffer_too_small_returns_error);
    RUN_TEST(serialize_null_buffer_returns_error);

    /* Group C: dispatch */
    RUN_TEST(dispatch_known_tool_calls_handler);
    RUN_TEST(dispatch_unknown_tool_publishes_error);
    RUN_TEST(dispatch_id_preserved_in_result);
    RUN_TEST(dispatch_success_publishes_ok_status);
    RUN_TEST(dispatch_handler_failure_publishes_error);
    RUN_TEST(dispatch_result_published_to_mcp_result_topic);
    RUN_TEST(dispatch_missing_tool_field_returns_error);

    /* Group D: publish */
    RUN_TEST(publish_mcp_tools_calls_mqtt_publish);
    RUN_TEST(publish_mcp_tools_uses_tools_list_topic);
    RUN_TEST(publish_mcp_tools_is_retained);
    RUN_TEST(publish_mcp_tools_skipped_when_disconnected);
    RUN_TEST(publish_mcp_tools_skipped_when_not_provisioned);

    /* Group E: init */
    RUN_TEST(mcp_init_subscribes_to_mcp_call);

    printf("\n═══════════════════════════════════════\n");
    printf("  Results: %d passed, %d failed\n", s_passed, s_failed);
    printf("═══════════════════════════════════════\n");

    return s_failed > 0 ? 1 : 0;
}
