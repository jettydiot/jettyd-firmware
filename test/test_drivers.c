/**
 * @file test_drivers.c
 * @brief Unit tests for the driver registry interface.
 *
 * Tests the driver registration, lookup, and capability matching
 * logic without real hardware.
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

/* ───────────────────────────── Mock ESP-IDF ───────────────────────────────── */

typedef int esp_err_t;
#define ESP_OK              0
#define ESP_FAIL            (-1)
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_NO_MEM      0x101
#define ESP_ERR_INVALID_STATE 0x103

#define ESP_LOGI(tag, fmt, ...) printf("[I] " fmt "\n", ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("[W] " fmt "\n", ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("[E] " fmt "\n", ##__VA_ARGS__)

/* Include driver header directly */
#include "jettyd_driver.h"

/* Include the driver_registry.c source for host testing */
/* In production this links via ESP-IDF components */

/* ───────────────────────────── Test Framework ─────────────────────────────── */

static int s_passed = 0;
static int s_failed = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("\n--- Test: %s ---\n", #name); \
    jettyd_driver_registry_init(); \
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

#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))
#define ASSERT_NULL(p) ASSERT_TRUE((p) == NULL)
#define ASSERT_NOT_NULL(p) ASSERT_TRUE((p) != NULL)

/* ───────────────────────────── Helper ─────────────────────────────────────── */

static jettyd_driver_t make_driver(const char *instance, const char *name,
                                    const char *cap_name, jettyd_capability_type_t cap_type)
{
    jettyd_driver_t drv = {0};
    strncpy(drv.instance, instance, JETTYD_MAX_INSTANCE_NAME - 1);
    strncpy(drv.driver_name, name, sizeof(drv.driver_name) - 1);
    drv.capability_count = 1;
    strncpy(drv.capabilities[0].name, cap_name, sizeof(drv.capabilities[0].name) - 1);
    drv.capabilities[0].type = cap_type;
    drv.capabilities[0].value_type = JETTYD_VAL_FLOAT;
    return drv;
}

/* ───────────────────────────── Tests ──────────────────────────────────────── */

TEST(register_and_find) {
    jettyd_driver_t drv = make_driver("soil", "soil_moisture", "moisture", JETTYD_CAP_READABLE);
    ASSERT_EQ(jettyd_driver_registry_add(&drv), ESP_OK);

    const jettyd_driver_t *found = jettyd_driver_find("soil");
    ASSERT_NOT_NULL(found);
    ASSERT_TRUE(strcmp(found->instance, "soil") == 0);
    ASSERT_TRUE(strcmp(found->driver_name, "soil_moisture") == 0);
}

TEST(find_returns_null_for_unknown) {
    const jettyd_driver_t *found = jettyd_driver_find("nonexistent");
    ASSERT_NULL(found);
}

TEST(find_capability_dotted_name) {
    jettyd_driver_t drv = make_driver("air", "dht22", "temperature", JETTYD_CAP_READABLE);
    jettyd_driver_registry_add(&drv);

    const jettyd_driver_t *found = jettyd_driver_find_capability("air.temperature");
    ASSERT_NOT_NULL(found);
    ASSERT_TRUE(strcmp(found->instance, "air") == 0);

    /* Non-existent capability on existing driver */
    found = jettyd_driver_find_capability("air.pressure");
    ASSERT_NULL(found);

    /* Non-existent driver */
    found = jettyd_driver_find_capability("water.temperature");
    ASSERT_NULL(found);
}

TEST(reject_duplicate_instance) {
    jettyd_driver_t drv1 = make_driver("valve", "relay", "state", JETTYD_CAP_SWITCHABLE);
    jettyd_driver_t drv2 = make_driver("valve", "relay", "state", JETTYD_CAP_SWITCHABLE);

    ASSERT_EQ(jettyd_driver_registry_add(&drv1), ESP_OK);
    ASSERT_EQ(jettyd_driver_registry_add(&drv2), ESP_ERR_INVALID_STATE);
}

TEST(driver_count) {
    ASSERT_EQ(jettyd_driver_count(), 0);

    jettyd_driver_t drv1 = make_driver("soil", "soil_moisture", "moisture", JETTYD_CAP_READABLE);
    jettyd_driver_t drv2 = make_driver("air", "dht22", "temperature", JETTYD_CAP_READABLE);

    jettyd_driver_registry_add(&drv1);
    ASSERT_EQ(jettyd_driver_count(), 1);

    jettyd_driver_registry_add(&drv2);
    ASSERT_EQ(jettyd_driver_count(), 2);
}

TEST(get_by_index) {
    jettyd_driver_t drv1 = make_driver("soil", "soil_moisture", "moisture", JETTYD_CAP_READABLE);
    jettyd_driver_t drv2 = make_driver("valve", "relay", "state", JETTYD_CAP_SWITCHABLE);

    jettyd_driver_registry_add(&drv1);
    jettyd_driver_registry_add(&drv2);

    const jettyd_driver_t *d0 = jettyd_driver_get(0);
    const jettyd_driver_t *d1 = jettyd_driver_get(1);
    const jettyd_driver_t *d2 = jettyd_driver_get(2);

    ASSERT_NOT_NULL(d0);
    ASSERT_NOT_NULL(d1);
    ASSERT_NULL(d2);
    ASSERT_TRUE(strcmp(d0->instance, "soil") == 0);
    ASSERT_TRUE(strcmp(d1->instance, "valve") == 0);
}

TEST(reject_null_driver) {
    ASSERT_EQ(jettyd_driver_registry_add(NULL), ESP_ERR_INVALID_ARG);
}

TEST(max_drivers) {
    /* Register JETTYD_MAX_DRIVERS drivers, then try one more */
    for (int i = 0; i < JETTYD_MAX_DRIVERS; i++) {
        char name[16];
        snprintf(name, sizeof(name), "drv%d", i);
        jettyd_driver_t drv = make_driver(name, "test", "val", JETTYD_CAP_READABLE);
        ASSERT_EQ(jettyd_driver_registry_add(&drv), ESP_OK);
    }

    jettyd_driver_t overflow = make_driver("overflow", "test", "val", JETTYD_CAP_READABLE);
    ASSERT_EQ(jettyd_driver_registry_add(&overflow), ESP_ERR_NO_MEM);
}

TEST(find_null_inputs) {
    ASSERT_NULL(jettyd_driver_find(NULL));
    ASSERT_NULL(jettyd_driver_find_capability(NULL));
    ASSERT_NULL(jettyd_driver_find_capability("nodot"));
}

/* ───────────────────────────── P1 Driver Tests ────────────────────────────── */

static jettyd_driver_t make_multi_cap_driver(const char *instance, const char *name,
                                              const char **cap_names,
                                              jettyd_capability_type_t cap_type,
                                              int cap_count)
{
    jettyd_driver_t drv = {0};
    strncpy(drv.instance, instance, JETTYD_MAX_INSTANCE_NAME - 1);
    strncpy(drv.driver_name, name, sizeof(drv.driver_name) - 1);
    drv.capability_count = (uint8_t)cap_count;
    for (int i = 0; i < cap_count && i < JETTYD_MAX_CAPABILITIES; i++) {
        strncpy(drv.capabilities[i].name, cap_names[i], sizeof(drv.capabilities[i].name) - 1);
        drv.capabilities[i].type = cap_type;
        drv.capabilities[i].value_type = JETTYD_VAL_FLOAT;
    }
    return drv;
}

TEST(bme280_register_three_capabilities) {
    const char *caps[] = {"temperature", "humidity", "pressure"};
    jettyd_driver_t drv = make_multi_cap_driver("env", "bme280", caps, JETTYD_CAP_READABLE, 3);

    drv.capabilities[0].min_value = -40;
    drv.capabilities[0].max_value = 85;
    drv.capabilities[1].min_value = 0;
    drv.capabilities[1].max_value = 100;
    drv.capabilities[2].min_value = 300;
    drv.capabilities[2].max_value = 1100;

    ASSERT_EQ(jettyd_driver_registry_add(&drv), ESP_OK);

    const jettyd_driver_t *found = jettyd_driver_find("env");
    ASSERT_NOT_NULL(found);
    ASSERT_EQ(found->capability_count, 3);
    ASSERT_TRUE(strcmp(found->driver_name, "bme280") == 0);

    /* Find each capability by dotted name */
    ASSERT_NOT_NULL(jettyd_driver_find_capability("env.temperature"));
    ASSERT_NOT_NULL(jettyd_driver_find_capability("env.humidity"));
    ASSERT_NOT_NULL(jettyd_driver_find_capability("env.pressure"));
    ASSERT_NULL(jettyd_driver_find_capability("env.altitude"));
}

TEST(pwm_output_writable_capability) {
    const char *caps[] = {"duty"};
    jettyd_driver_t drv = make_multi_cap_driver("fan", "pwm_output", caps, JETTYD_CAP_WRITABLE, 1);

    drv.capabilities[0].min_value = 0;
    drv.capabilities[0].max_value = 100;

    ASSERT_EQ(jettyd_driver_registry_add(&drv), ESP_OK);

    const jettyd_driver_t *found = jettyd_driver_find("fan");
    ASSERT_NOT_NULL(found);
    ASSERT_EQ(found->capability_count, 1);
    ASSERT_EQ(found->capabilities[0].type, JETTYD_CAP_WRITABLE);
    ASSERT_TRUE(strcmp(found->capabilities[0].name, "duty") == 0);
}

TEST(hcsr04_readable_distance) {
    const char *caps[] = {"distance"};
    jettyd_driver_t drv = make_multi_cap_driver("range", "hcsr04", caps, JETTYD_CAP_READABLE, 1);

    drv.capabilities[0].min_value = 2;
    drv.capabilities[0].max_value = 400;

    ASSERT_EQ(jettyd_driver_registry_add(&drv), ESP_OK);

    const jettyd_driver_t *found = jettyd_driver_find_capability("range.distance");
    ASSERT_NOT_NULL(found);
    ASSERT_TRUE(strcmp(found->driver_name, "hcsr04") == 0);
}

TEST(ina219_register_three_capabilities) {
    const char *caps[] = {"voltage", "current", "power"};
    jettyd_driver_t drv = make_multi_cap_driver("pwr", "ina219", caps, JETTYD_CAP_READABLE, 3);

    drv.capabilities[0].min_value = 0;
    drv.capabilities[0].max_value = 26;
    drv.capabilities[1].min_value = -3.2f;
    drv.capabilities[1].max_value = 3.2f;
    drv.capabilities[2].min_value = 0;
    drv.capabilities[2].max_value = 83.2f;

    ASSERT_EQ(jettyd_driver_registry_add(&drv), ESP_OK);

    const jettyd_driver_t *found = jettyd_driver_find("pwr");
    ASSERT_NOT_NULL(found);
    ASSERT_EQ(found->capability_count, 3);
    ASSERT_TRUE(strcmp(found->driver_name, "ina219") == 0);

    ASSERT_NOT_NULL(jettyd_driver_find_capability("pwr.voltage"));
    ASSERT_NOT_NULL(jettyd_driver_find_capability("pwr.current"));
    ASSERT_NOT_NULL(jettyd_driver_find_capability("pwr.power"));
    ASSERT_NULL(jettyd_driver_find_capability("pwr.resistance"));
}

TEST(p1_drivers_coexist) {
    /* Register all four P1 drivers and verify they coexist */
    const char *bme_caps[] = {"temperature", "humidity", "pressure"};
    const char *pwm_caps[] = {"duty"};
    const char *hcsr_caps[] = {"distance"};
    const char *ina_caps[] = {"voltage", "current", "power"};

    jettyd_driver_t bme = make_multi_cap_driver("env", "bme280", bme_caps, JETTYD_CAP_READABLE, 3);
    jettyd_driver_t pwm = make_multi_cap_driver("fan", "pwm_output", pwm_caps, JETTYD_CAP_WRITABLE, 1);
    jettyd_driver_t hcsr = make_multi_cap_driver("range", "hcsr04", hcsr_caps, JETTYD_CAP_READABLE, 1);
    jettyd_driver_t ina = make_multi_cap_driver("pwr", "ina219", ina_caps, JETTYD_CAP_READABLE, 3);

    ASSERT_EQ(jettyd_driver_registry_add(&bme), ESP_OK);
    ASSERT_EQ(jettyd_driver_registry_add(&pwm), ESP_OK);
    ASSERT_EQ(jettyd_driver_registry_add(&hcsr), ESP_OK);
    ASSERT_EQ(jettyd_driver_registry_add(&ina), ESP_OK);

    ASSERT_EQ(jettyd_driver_count(), 4);

    /* Cross-driver capability lookups */
    ASSERT_NOT_NULL(jettyd_driver_find_capability("env.pressure"));
    ASSERT_NOT_NULL(jettyd_driver_find_capability("fan.duty"));
    ASSERT_NOT_NULL(jettyd_driver_find_capability("range.distance"));
    ASSERT_NOT_NULL(jettyd_driver_find_capability("pwr.power"));

    /* No cross-contamination */
    ASSERT_NULL(jettyd_driver_find_capability("env.duty"));
    ASSERT_NULL(jettyd_driver_find_capability("fan.temperature"));
}

/* ───────────────────────────── Main ───────────────────────────────────────── */

int main(void)
{
    printf("═══════════════════════════════════════\n");
    printf("  Jettyd Driver Registry Unit Tests\n");
    printf("═══════════════════════════════════════\n");

    RUN_TEST(register_and_find);
    RUN_TEST(find_returns_null_for_unknown);
    RUN_TEST(find_capability_dotted_name);
    RUN_TEST(reject_duplicate_instance);
    RUN_TEST(driver_count);
    RUN_TEST(get_by_index);
    RUN_TEST(reject_null_driver);
    RUN_TEST(max_drivers);
    RUN_TEST(find_null_inputs);

    /* P1 driver tests */
    RUN_TEST(bme280_register_three_capabilities);
    RUN_TEST(pwm_output_writable_capability);
    RUN_TEST(hcsr04_readable_distance);
    RUN_TEST(ina219_register_three_capabilities);
    RUN_TEST(p1_drivers_coexist);

    printf("\n═══════════════════════════════════════\n");
    printf("  Results: %d passed, %d failed\n", s_passed, s_failed);
    printf("═══════════════════════════════════════\n");

    return s_failed > 0 ? 1 : 0;
}
