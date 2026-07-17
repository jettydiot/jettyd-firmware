/**
 * @file test_servo.c
 * @brief Unit tests for the servo driver logic.
 *
 * Tests pulse-width math (angle -> pulse_us, clamping) and rotate command
 * param parsing/defaults directly against the driver's static state —
 * without real hardware or FreeRTOS timers.
 */

#include "mocks/esp_idf_stubs.h"

#include "jettyd_driver.h"

/* Stub registry — servo.c calls jettyd_driver_registry_add via JETTYD_REGISTER_DRIVER */
esp_err_t jettyd_driver_registry_add(const jettyd_driver_t *drv) { (void)drv; return ESP_OK; }

/* ── Pull in servo source directly for white-box testing ───────────────────── */
#include "servo.h"
#include "../drivers/servo/servo.c"

/* ── Test framework ──────────────────────────────────────────────────────── */

static int s_passed = 0;
static int s_failed = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("\n--- Test: %s ---\n", #name); \
    _reset_servo_state(); \
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
#define ASSERT_NEAR(a, b, eps) ASSERT_TRUE(((a) - (b) < (eps)) && ((b) - (a) < (eps)))

static void _reset_servo_state(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    memset(&s_driver, 0, sizeof(s_driver));
    s_current_angle = 0.0f;
    s_attached = false;
}

static void _default_cfg(void)
{
    s_cfg.pin = 2;
    s_cfg.ledc_channel = 0;
    s_cfg.min_pulse_us = 500;
    s_cfg.max_pulse_us = 2500;
    s_cfg.max_angle = 180.0f;
    s_cfg.home_angle = 0.0f;
    s_cfg.idle_detach = true;
}

/* ── Pulse-width math ────────────────────────────────────────────────────── */

TEST(angle_zero_maps_to_min_pulse) {
    _default_cfg();
    ASSERT_EQ(angle_to_pulse_us(0.0f), 500u);
}

TEST(max_angle_maps_to_max_pulse) {
    _default_cfg();
    ASSERT_EQ(angle_to_pulse_us(180.0f), 2500u);
}

TEST(mid_angle_maps_to_mid_pulse) {
    _default_cfg();
    ASSERT_EQ(angle_to_pulse_us(90.0f), 1500u);
}

TEST(negative_angle_clamps_to_min_pulse) {
    _default_cfg();
    ASSERT_EQ(angle_to_pulse_us(-45.0f), 500u);
}

TEST(over_max_angle_clamps_to_max_pulse) {
    _default_cfg();
    ASSERT_EQ(angle_to_pulse_us(999.0f), 2500u);
}

TEST(custom_pulse_range_respected) {
    _default_cfg();
    s_cfg.min_pulse_us = 1000;
    s_cfg.max_pulse_us = 2000;
    s_cfg.max_angle = 90.0f;

    ASSERT_EQ(angle_to_pulse_us(0.0f), 1000u);
    ASSERT_EQ(angle_to_pulse_us(90.0f), 2000u);
    ASSERT_EQ(angle_to_pulse_us(45.0f), 1500u);
}

TEST(clamp_angle_bounds) {
    _default_cfg();
    ASSERT_NEAR(clamp_angle(-10.0f), 0.0f, 0.001f);
    ASSERT_NEAR(clamp_angle(200.0f), 180.0f, 0.001f);
    ASSERT_NEAR(clamp_angle(90.0f), 90.0f, 0.001f);
}

/* ── move_to / write clamping ───────────────────────────────────────────── */

TEST(move_to_clamps_and_updates_current_angle) {
    _default_cfg();
    move_to(500.0f);
    ASSERT_NEAR(s_current_angle, 180.0f, 0.001f);

    move_to(-50.0f);
    ASSERT_NEAR(s_current_angle, 0.0f, 0.001f);
}

TEST(write_angle_capability_clamps) {
    _default_cfg();
    jettyd_value_t v = { .type = JETTYD_VAL_FLOAT, .float_val = 270.0f, .valid = true };
    ASSERT_EQ(servo_write("angle", v), ESP_OK);
    ASSERT_NEAR(s_current_angle, 180.0f, 0.001f);
}

TEST(write_unknown_capability_rejected) {
    _default_cfg();
    jettyd_value_t v = { .type = JETTYD_VAL_FLOAT, .float_val = 10.0f, .valid = true };
    ASSERT_EQ(servo_write("speed", v), ESP_ERR_NOT_SUPPORTED);
}

/* ── rotate command parsing/defaults ────────────────────────────────────── */

TEST(rotate_command_defaults_to_90_degrees) {
    _default_cfg();
    ASSERT_EQ(servo_command("rotate", NULL), ESP_OK);
    ASSERT_NEAR(s_current_angle, 90.0f, 0.001f);
}

TEST(rotate_command_parses_angle_param) {
    _default_cfg();
    ASSERT_EQ(servo_command("rotate", "{\"angle\": 45, \"hold_ms\": 200}"), ESP_OK);
    ASSERT_NEAR(s_current_angle, 45.0f, 0.001f);
}

TEST(rotate_command_clamps_out_of_range_angle) {
    _default_cfg();
    ASSERT_EQ(servo_command("rotate", "{\"angle\": 999}"), ESP_OK);
    ASSERT_NEAR(s_current_angle, 180.0f, 0.001f);
}

TEST(rotate_command_missing_hold_ms_uses_default) {
    _default_cfg();
    /* Only angle present — hold_ms should fall back to 1000ms internally.
       We can't observe the timer on host stubs, but the move itself must
       still apply immediately regardless of hold_ms parsing. */
    ASSERT_EQ(servo_command("rotate", "{\"angle\": 30}"), ESP_OK);
    ASSERT_NEAR(s_current_angle, 30.0f, 0.001f);
}

TEST(unknown_command_rejected) {
    _default_cfg();
    ASSERT_EQ(servo_command("led.blink", NULL), ESP_ERR_NOT_SUPPORTED);
}

/* ── read ────────────────────────────────────────────────────────────────── */

TEST(read_returns_current_angle) {
    _default_cfg();
    move_to(60.0f);
    jettyd_value_t v = servo_read("angle");
    ASSERT_TRUE(v.valid);
    ASSERT_EQ(v.type, JETTYD_VAL_FLOAT);
    ASSERT_NEAR(v.float_val, 60.0f, 0.001f);
}

/* ── registration ────────────────────────────────────────────────────────── */

TEST(register_sets_capability_and_command_hook) {
    servo_config_t cfg = {
        .pin = 2, .ledc_channel = 0,
        .min_pulse_us = 500, .max_pulse_us = 2500,
        .max_angle = 180.0f, .home_angle = 0.0f,
        .idle_detach = true,
    };
    servo_register("gate", &cfg);

    ASSERT_TRUE(strcmp(s_driver.instance, "gate") == 0);
    ASSERT_TRUE(strcmp(s_driver.driver_name, "servo") == 0);
    ASSERT_EQ(s_driver.capability_count, 1);
    ASSERT_TRUE(strcmp(s_driver.capabilities[0].name, "angle") == 0);
    ASSERT_EQ(s_driver.capabilities[0].type, JETTYD_CAP_WRITABLE);
    ASSERT_NEAR(s_driver.capabilities[0].max_value, 180.0f, 0.001f);
    ASSERT_TRUE(s_driver.command == servo_command);
    ASSERT_TRUE(s_driver.write == servo_write);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("═══════════════════════════════════════\n");
    printf("  Servo Driver Unit Tests\n");
    printf("═══════════════════════════════════════\n");

    RUN_TEST(angle_zero_maps_to_min_pulse);
    RUN_TEST(max_angle_maps_to_max_pulse);
    RUN_TEST(mid_angle_maps_to_mid_pulse);
    RUN_TEST(negative_angle_clamps_to_min_pulse);
    RUN_TEST(over_max_angle_clamps_to_max_pulse);
    RUN_TEST(custom_pulse_range_respected);
    RUN_TEST(clamp_angle_bounds);
    RUN_TEST(move_to_clamps_and_updates_current_angle);
    RUN_TEST(write_angle_capability_clamps);
    RUN_TEST(write_unknown_capability_rejected);
    RUN_TEST(rotate_command_defaults_to_90_degrees);
    RUN_TEST(rotate_command_parses_angle_param);
    RUN_TEST(rotate_command_clamps_out_of_range_angle);
    RUN_TEST(rotate_command_missing_hold_ms_uses_default);
    RUN_TEST(unknown_command_rejected);
    RUN_TEST(read_returns_current_angle);
    RUN_TEST(register_sets_capability_and_command_hook);

    printf("\n═══════════════════════════════════════\n");
    printf("  Results: %d passed, %d failed\n", s_passed, s_failed);
    printf("═══════════════════════════════════════\n");

    return s_failed > 0 ? 1 : 0;
}
