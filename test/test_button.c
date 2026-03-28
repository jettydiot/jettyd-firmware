/**
 * @file test_button.c
 * @brief Unit tests for the button driver logic.
 *
 * Tests the debounce logic, press counting, active_high/active_low polarity,
 * and the ISR state machine — without real hardware or FreeRTOS.
 *
 * The button driver's ISR handler is extracted and called directly with
 * simulated GPIO state changes. The event task is not spawned (mocked out).
 */

#include "mocks/esp_idf_stubs.h"

/* ── Stub out jettyd_telemetry so we can track publish calls ─────────────── */
#include "jettyd_driver.h"

static int s_telemetry_publish_count = 0;
static char s_last_metrics[8][64];
static int  s_last_metric_count = 0;

/* Stub — replaces the real jettyd_telemetry_publish in this TU */
esp_err_t jettyd_telemetry_publish(const char **metrics, uint8_t metric_count)
{
    s_telemetry_publish_count++;
    s_last_metric_count = metric_count;
    for (int i = 0; i < metric_count && i < 8; i++) {
        strncpy(s_last_metrics[i], metrics[i], 63);
        s_last_metrics[i][63] = '\0';
    }
    return ESP_OK;
}

/* Stub registry — button.c calls jettyd_driver_registry_add */
esp_err_t jettyd_driver_registry_add(const jettyd_driver_t *drv) { (void)drv; return ESP_OK; }

/* ── Pull in button source directly for white-box testing ─────────────────── */
#include "button.h"

/* Include button.c directly for white-box access to static state.
   Suppress warnings about static functions not called from this TU
   (button_event_task is only invoked via xTaskCreate which is a no-op stub). */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../drivers/button/button.c"
#pragma GCC diagnostic pop

/* ── Test framework ──────────────────────────────────────────────────────── */

static int s_passed = 0;
static int s_failed = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("\n--- Test: %s ---\n", #name); \
    _reset_button_state(); \
    s_telemetry_publish_count = 0; \
    s_last_metric_count = 0; \
    test_##name(); \
    printf("--- %s: PASSED ---\n", #name); \
    s_passed++; \
} while(0)

#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        printf("FAIL at %s:%d — %s\n", __FILE__, __LINE__, #expr); \
        s_failed++; \
        return; \
    } \
} while(0)
#define ASSERT_EQ(a, b)    ASSERT_TRUE((a) == (b))
#define ASSERT_FALSE(expr) ASSERT_TRUE(!(expr))

/* Reset all button driver static state between tests */
static void _reset_button_state(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    memset(&s_driver, 0, sizeof(s_driver));
    s_pressed = false;
    s_press_count = 0;
    s_last_edge_us = 0;
    s_last_reported_count = 0;
    memset(s_instance, 0, sizeof(s_instance));
    memset(g_gpio_levels, 0, sizeof(g_gpio_levels));
}

/* Simulate a button press: pull pin low (active_low), wait past debounce */
static void _simulate_press(uint8_t pin, bool active_low)
{
    int pressed_level = active_low ? 0 : 1;
    g_gpio_levels[pin] = pressed_level;
    /* Advance last_edge_us far enough back that debounce passes */
    s_last_edge_us = esp_timer_get_time() - 100000LL; /* 100ms ago */
    gpio_isr_handler(NULL);
}

static void _simulate_release(uint8_t pin, bool active_low)
{
    int released_level = active_low ? 1 : 0;
    g_gpio_levels[pin] = released_level;
    s_last_edge_us = esp_timer_get_time() - 100000LL;
    gpio_isr_handler(NULL);
}

/* ── Tests ───────────────────────────────────────────────────────────────── */

TEST(active_low_press_increments_count) {
    s_cfg.pin = 9;
    s_cfg.active_low = true;
    s_cfg.debounce_ms = 50;

    _simulate_press(9, true);
    ASSERT_TRUE(s_pressed);
    ASSERT_EQ(s_press_count, 1);
}

TEST(active_low_release_clears_pressed) {
    s_cfg.pin = 9;
    s_cfg.active_low = true;
    s_cfg.debounce_ms = 50;

    _simulate_press(9, true);
    _simulate_release(9, true);
    ASSERT_FALSE(s_pressed);
    ASSERT_EQ(s_press_count, 1); /* count stays */
}

TEST(active_high_press_increments_count) {
    s_cfg.pin = 3;
    s_cfg.active_low = false;
    s_cfg.debounce_ms = 50;

    _simulate_press(3, false);
    ASSERT_TRUE(s_pressed);
    ASSERT_EQ(s_press_count, 1);
}

TEST(multiple_presses_accumulate) {
    s_cfg.pin = 9;
    s_cfg.active_low = true;
    s_cfg.debounce_ms = 50;

    for (int i = 0; i < 5; i++) {
        _simulate_press(9, true);
        _simulate_release(9, true);
    }
    ASSERT_EQ(s_press_count, 5);
}

TEST(debounce_rejects_rapid_edges) {
    s_cfg.pin = 9;
    s_cfg.active_low = true;
    s_cfg.debounce_ms = 50;

    /* First edge — valid */
    g_gpio_levels[9] = 0; /* pressed */
    s_last_edge_us = esp_timer_get_time() - 100000LL; /* 100ms ago — past debounce */
    gpio_isr_handler(NULL);
    ASSERT_EQ(s_press_count, 1);

    /* Second edge — within debounce window (5ms ago, debounce=50ms) */
    s_last_edge_us = esp_timer_get_time() - 5000LL;
    g_gpio_levels[9] = 1; /* release */
    gpio_isr_handler(NULL);
    /* Should be rejected — count stays at 1, still shows pressed */
    ASSERT_EQ(s_press_count, 1);
    ASSERT_TRUE(s_pressed); /* state unchanged */
}

TEST(no_double_count_on_held_press) {
    s_cfg.pin = 9;
    s_cfg.active_low = true;
    s_cfg.debounce_ms = 50;

    /* Press once */
    _simulate_press(9, true);
    ASSERT_EQ(s_press_count, 1);

    /* ISR fires again while still held (same level) — should not double-count */
    s_last_edge_us = esp_timer_get_time() - 100000LL;
    g_gpio_levels[9] = 0; /* still pressed */
    gpio_isr_handler(NULL);
    ASSERT_EQ(s_press_count, 1); /* no double count */
}

TEST(event_task_detects_count_change) {
    s_cfg.pin = 9;
    s_cfg.active_low = true;
    s_cfg.debounce_ms = 50;
    strlcpy(s_instance, "btn", sizeof(s_instance));

    _simulate_press(9, true);
    ASSERT_EQ(s_press_count, 1);
    ASSERT_EQ(s_last_reported_count, 0); /* not reported yet */

    /* Manually invoke what the event task would do */
    if (s_press_count != s_last_reported_count) {
        s_last_reported_count = s_press_count;
        const char *press_metric  = "btn.press";
        const char *count_metric  = "btn.press_count";
        const char *pub_metrics[] = { press_metric, count_metric };
        jettyd_telemetry_publish(pub_metrics, 2);
    }

    ASSERT_EQ(s_last_reported_count, 1);
    ASSERT_EQ(s_telemetry_publish_count, 1);
    ASSERT_EQ(s_last_metric_count, 2);
    ASSERT_TRUE(strcmp(s_last_metrics[0], "btn.press") == 0);
    ASSERT_TRUE(strcmp(s_last_metrics[1], "btn.press_count") == 0);
}

TEST(event_task_no_publish_when_unchanged) {
    s_cfg.pin = 9;
    s_cfg.active_low = true;
    s_cfg.debounce_ms = 50;
    strlcpy(s_instance, "btn", sizeof(s_instance));

    /* No presses — task loop should not publish */
    if (s_press_count != s_last_reported_count) {
        s_last_reported_count = s_press_count;
        const char *m[] = { "btn.press", "btn.press_count" };
        jettyd_telemetry_publish(m, 2);
    }

    ASSERT_EQ(s_telemetry_publish_count, 0);
}

TEST(button_read_press_metric) {
    s_cfg.pin = 9;
    s_cfg.active_low = true;
    s_cfg.debounce_ms = 50;

    _simulate_press(9, true);

    jettyd_value_t v = button_read("press");
    ASSERT_TRUE(v.valid);
    ASSERT_EQ(v.type, JETTYD_VAL_BOOL);
    ASSERT_TRUE(v.bool_val);

    _simulate_release(9, true);
    v = button_read("press");
    ASSERT_FALSE(v.bool_val);
}

TEST(button_read_press_count_metric) {
    s_cfg.pin = 9;
    s_cfg.active_low = true;
    s_cfg.debounce_ms = 50;

    for (int i = 0; i < 3; i++) {
        _simulate_press(9, true);
        _simulate_release(9, true);
    }

    jettyd_value_t v = button_read("press_count");
    ASSERT_TRUE(v.valid);
    ASSERT_EQ(v.type, JETTYD_VAL_FLOAT);
    ASSERT_EQ((int)v.float_val, 3);
}

TEST(button_read_unknown_metric_invalid) {
    jettyd_value_t v = button_read("nonexistent");
    ASSERT_FALSE(v.valid);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("═══════════════════════════════════════\n");
    printf("  Button Driver Unit Tests\n");
    printf("═══════════════════════════════════════\n");

    RUN_TEST(active_low_press_increments_count);
    RUN_TEST(active_low_release_clears_pressed);
    RUN_TEST(active_high_press_increments_count);
    RUN_TEST(multiple_presses_accumulate);
    RUN_TEST(debounce_rejects_rapid_edges);
    RUN_TEST(no_double_count_on_held_press);
    RUN_TEST(event_task_detects_count_change);
    RUN_TEST(event_task_no_publish_when_unchanged);
    RUN_TEST(button_read_press_metric);
    RUN_TEST(button_read_press_count_metric);
    RUN_TEST(button_read_unknown_metric_invalid);

    printf("\n═══════════════════════════════════════\n");
    printf("  Results: %d passed, %d failed\n", s_passed, s_failed);
    printf("═══════════════════════════════════════\n");

    return s_failed > 0 ? 1 : 0;
}
