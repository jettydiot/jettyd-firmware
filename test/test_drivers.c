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

    printf("\n═══════════════════════════════════════\n");
    printf("  Results: %d passed, %d failed\n", s_passed, s_failed);
    printf("═══════════════════════════════════════\n");

    return s_failed > 0 ? 1 : 0;
}
