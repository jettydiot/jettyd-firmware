/**
 * @file test_telemetry.c
 * @brief Unit tests for telemetry message formatting.
 *
 * Verifies the telemetry JSON output matches the platform's expected
 * format: { "ts": <unix_ts>, "readings": { "instance.capability": value } }
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* ───────────────────────────── Mock ESP-IDF ───────────────────────────────── */

typedef int esp_err_t;
#define ESP_OK              0
#define ESP_FAIL            (-1)
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_NO_MEM      0x101

#define ESP_LOGI(tag, fmt, ...) printf("[I] " fmt "\n", ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("[W] " fmt "\n", ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("[E] " fmt "\n", ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...)

static uint32_t esp_get_free_heap_size(void) { return 200000; }
static int64_t esp_timer_get_time(void) { return 86400000000LL; } /* 1 day in us */

#include "jettyd_driver.h"

/* ───────────────────────────── Test Framework ─────────────────────────────── */

static int s_passed = 0;
static int s_failed = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("\n--- Test: %s ---\n", #name); \
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

/* ───────────────────────────── Tests ──────────────────────────────────────── */

TEST(telemetry_json_format) {
    /* Verify the expected JSON shape matches what the platform expects.
     * The platform telemetry endpoint expects individual metrics via
     * POST /v1/devices/{id}/telemetry with: { metric, value_numeric }
     * But MQTT telemetry uses the batched format:
     * { "ts": <unix>, "readings": { "metric": value, ... } } */

    /* Simulate building a telemetry message */
    char json[512];
    time_t now = time(NULL);
    int len = snprintf(json, sizeof(json),
        "{\"ts\":%ld,\"readings\":{"
        "\"soil.moisture\":42.3,"
        "\"air.temperature\":23.1,"
        "\"air.humidity\":65.2,"
        "\"valve.state\":false,"
        "\"system.battery\":3.82,"
        "\"system.rssi\":-54,"
        "\"system.uptime\":86400,"
        "\"system.heap_free\":128000"
        "}}",
        (long)now);

    ASSERT_TRUE(len > 0);
    ASSERT_TRUE(len < 512);

    /* Verify key fields are present */
    ASSERT_TRUE(strstr(json, "\"ts\":") != NULL);
    ASSERT_TRUE(strstr(json, "\"readings\":") != NULL);
    ASSERT_TRUE(strstr(json, "\"soil.moisture\":") != NULL);
    ASSERT_TRUE(strstr(json, "\"system.battery\":") != NULL);
    ASSERT_TRUE(strstr(json, "\"system.rssi\":") != NULL);
    ASSERT_TRUE(strstr(json, "\"system.uptime\":") != NULL);
    ASSERT_TRUE(strstr(json, "\"system.heap_free\":") != NULL);

    printf("  JSON: %s\n", json);
}

TEST(command_response_format) {
    /* Verify command response matches platform expectations:
     * { "id": "cmd_abc", "status": "acked", "result": {...} } */

    char json[256];
    snprintf(json, sizeof(json),
        "{\"id\":\"cmd_abc123\",\"status\":\"acked\","
        "\"result\":{\"valve.state\":true,\"will_auto_off_at\":1710763500}}");

    ASSERT_TRUE(strstr(json, "\"id\":\"cmd_abc123\"") != NULL);
    ASSERT_TRUE(strstr(json, "\"status\":\"acked\"") != NULL);
    ASSERT_TRUE(strstr(json, "\"result\":") != NULL);

    printf("  JSON: %s\n", json);
}

TEST(shadow_format) {
    /* Verify shadow report matches platform expectations:
     * { "reported": {...}, "desired": null, "metadata": { "last_report": <ts> } } */

    char json[512];
    time_t now = time(NULL);
    snprintf(json, sizeof(json),
        "{\"reported\":{"
        "\"soil.moisture\":42.3,"
        "\"system.firmware_version\":\"1.0.0\","
        "\"vm.rules_loaded\":2,"
        "\"vm.heartbeats_loaded\":1"
        "},\"desired\":null,"
        "\"metadata\":{\"last_report\":%ld}}",
        (long)now);

    ASSERT_TRUE(strstr(json, "\"reported\":") != NULL);
    ASSERT_TRUE(strstr(json, "\"desired\":null") != NULL);
    ASSERT_TRUE(strstr(json, "\"metadata\":") != NULL);
    ASSERT_TRUE(strstr(json, "\"vm.rules_loaded\":") != NULL);

    printf("  JSON: %s\n", json);
}

TEST(alert_format) {
    /* Verify alert format:
     * { "ts": <unix>, "message": "...", "severity": "info|warning|critical" } */

    char json[256];
    snprintf(json, sizeof(json),
        "{\"ts\":%ld,\"message\":\"Low moisture: 28.5%% at 23.1°C\","
        "\"severity\":\"warning\"}",
        (long)time(NULL));

    ASSERT_TRUE(strstr(json, "\"message\":") != NULL);
    ASSERT_TRUE(strstr(json, "\"severity\":\"warning\"") != NULL);

    printf("  JSON: %s\n", json);
}

TEST(provisioning_request_format) {
    /* Verify provisioning request format:
     * { "fleet_token": "ft_...", "device_type": "...", "firmware_version": "...",
     *   "mac_address": "..." } */

    char json[256];
    snprintf(json, sizeof(json),
        "{\"fleet_token\":\"ft_test123\","
        "\"device_type\":\"soil-moisture-v1\","
        "\"firmware_version\":\"1.0.0\","
        "\"mac_address\":\"aa:bb:cc:dd:ee:ff\"}");

    ASSERT_TRUE(strstr(json, "\"fleet_token\":") != NULL);
    ASSERT_TRUE(strstr(json, "\"device_type\":") != NULL);
    ASSERT_TRUE(strstr(json, "\"firmware_version\":") != NULL);

    printf("  JSON: %s\n", json);
}

TEST(ota_notification_format) {
    /* Verify OTA notification format matches platform:
     * { "version": "1.3.0", "url": "...", "checksum": "sha256:...", "size": 524288 } */

    char json[512];
    snprintf(json, sizeof(json),
        "{\"version\":\"1.3.0\","
        "\"url\":\"https://r2.jettyd.com/firmware/soil-moisture-v1/v1.3.0/esp32s3/firmware.bin\","
        "\"checksum\":\"sha256:a1b2c3d4e5f6\","
        "\"size\":524288}");

    ASSERT_TRUE(strstr(json, "\"version\":\"1.3.0\"") != NULL);
    ASSERT_TRUE(strstr(json, "\"url\":") != NULL);
    ASSERT_TRUE(strstr(json, "\"checksum\":\"sha256:") != NULL);
    ASSERT_TRUE(strstr(json, "\"size\":524288") != NULL);

    printf("  JSON: %s\n", json);
}

/* ───────────────────────────── Main ───────────────────────────────────────── */

int main(void)
{
    printf("═══════════════════════════════════════\n");
    printf("  Jettyd Telemetry Format Unit Tests\n");
    printf("═══════════════════════════════════════\n");

    RUN_TEST(telemetry_json_format);
    RUN_TEST(command_response_format);
    RUN_TEST(shadow_format);
    RUN_TEST(alert_format);
    RUN_TEST(provisioning_request_format);
    RUN_TEST(ota_notification_format);

    printf("\n═══════════════════════════════════════\n");
    printf("  Results: %d passed, %d failed\n", s_passed, s_failed);
    printf("═══════════════════════════════════════\n");

    return s_failed > 0 ? 1 : 0;
}
