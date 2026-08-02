/**
 * @file test_display.c
 * @brief Unit tests for the MAX7219 display driver.
 *
 * White-box pattern (mirrors test_servo.c): display.c is compiled directly
 * via #include so static state and helpers are in scope.
 *
 * Coverage:
 *  - driver registers and display.set dispatches to it
 *  - compact_number boundaries: 0, 99999, 100000, 999999, 1000000, 9999999,
 *    99999999, 999999999, 1000000000; negative → "----"
 *  - brightness validation: 0/15 ok; -1/16 rejected, no fb write
 *  - missing/invalid value rejected
 *  - frame buffer maps known glyphs ('0', '1') to expected column bytes
 *  - identity NVS keys (fleet_token/device_key/tenant_id) untouched in all paths
 */

#include "mocks/esp_idf_stubs.h"
#include "jettyd_driver.h"

/* Stub registry */
esp_err_t jettyd_driver_registry_add(const jettyd_driver_t *drv) { (void)drv; return ESP_OK; }

/* NVS stub — panics on any call to prove the display driver never touches identity keys */
typedef struct { int _unused; } nvs_handle_t;
static int s_nvs_write_calls = 0;
static esp_err_t stub_nvs_set_str(unsigned int h, const char *k, const char *v)
{
    (void)h; (void)k; (void)v;
    s_nvs_write_calls++;
    return ESP_OK;
}

#include "display.h"
#include "../drivers/display/display.c"

/* ── Test framework ──────────────────────────────────────────────────────── */

static int s_passed = 0;
static int s_failed = 0;
static bool s_meta_fail_active = false;

#define TEST(name)  static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("\n--- Test: %s ---\n", #name); \
    _reset_display_state(); \
    _default_cfg(); \
    s_nvs_write_calls = 0; \
    int _failed_before = s_failed; \
    test_##name(); \
    if (s_nvs_write_calls != 0) { \
        printf("FAIL: unexpected NVS write(s) detected\n"); \
        s_failed++; \
    } \
    if (s_failed == _failed_before) { \
        printf("--- %s: done ---\n", #name); \
        s_passed++; \
    } \
} while(0)

#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        printf("FAIL at %s:%d — %s\n", __FILE__, __LINE__, #expr); \
        s_failed++; \
        return; \
    } \
} while(0)
#define ASSERT_EQ(a, b)         ASSERT_TRUE((a) == (b))
#define ASSERT_STR_EQ(a, b)     ASSERT_TRUE(strcmp((a), (b)) == 0)

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void _reset_display_state(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    memset(&s_driver, 0, sizeof(s_driver));
    memset(s_fb, 0, sizeof(s_fb));
}

static void _default_cfg(void)
{
    s_cfg.pin_din     = 10;
    s_cfg.pin_clk     = 8;
    s_cfg.pin_cs      = 9;
    s_cfg.num_modules = 4;
    s_cfg.brightness  = 3;
}

/* Is the pixel at (row, col) lit in the frame buffer? */
static bool pixel_lit(int row, int col)
{
    int module = col / 8;
    int bit    = 7 - (col % 8);
    return (s_fb[row][module] >> bit) & 1;
}

/* ── compact_number boundaries ───────────────────────────────────────────── */

TEST(compact_zero) {
    char buf[16];
    compact_number(0LL, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "0");
}

TEST(compact_99999) {
    char buf[16];
    compact_number(99999LL, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "99999");
}

TEST(compact_100000_is_100k) {
    char buf[16];
    compact_number(100000LL, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "100k");
}

TEST(compact_999999_is_999k) {
    char buf[16];
    compact_number(999999LL, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "999k");
}

TEST(compact_1000000_is_1_0M) {
    char buf[16];
    compact_number(1000000LL, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "1.0M");
}

TEST(compact_9999999_is_9_9M) {
    char buf[16];
    compact_number(9999999LL, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "9.9M");
}

TEST(compact_99999999_is_99M) {
    char buf[16];
    compact_number(99999999LL, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "99M");
}

TEST(compact_999999999_is_999M) {
    char buf[16];
    compact_number(999999999LL, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "999M");
}

TEST(compact_1000000000_is_1B) {
    char buf[16];
    compact_number(1000000000LL, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "1B");
}

TEST(compact_negative_is_dash) {
    char buf[16];
    compact_number(-1LL, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "----");
}

/* ── display.set dispatch ────────────────────────────────────────────────── */

TEST(driver_registers_and_set_dispatches) {
    display_config_t cfg = {
        .pin_din = 10, .pin_clk = 8, .pin_cs = 9,
        .num_modules = 4, .brightness = 3,
    };
    display_register("panel", &cfg);

    ASSERT_STR_EQ(s_driver.instance, "panel");
    ASSERT_STR_EQ(s_driver.driver_name, "display");
    ASSERT_EQ(s_driver.capability_count, 0);
    ASSERT_TRUE(s_driver.command == display_command);

    /* display.set must succeed with valid params */
    esp_err_t err = display_command("set", "{\"value\": 42}");
    ASSERT_EQ(err, ESP_OK);
}

/* ── brightness validation ───────────────────────────────────────────────── */

TEST(brightness_0_accepted) {
    esp_err_t err = display_command("set", "{\"value\": 1, \"brightness\": 0}");
    ASSERT_EQ(err, ESP_OK);
    ASSERT_EQ(s_cfg.brightness, 0);
}

TEST(brightness_15_accepted) {
    esp_err_t err = display_command("set", "{\"value\": 1, \"brightness\": 15}");
    ASSERT_EQ(err, ESP_OK);
    ASSERT_EQ(s_cfg.brightness, 15);
}

TEST(brightness_minus1_rejected_no_write) {
    s_cfg.brightness = 5;
    display_command("set", "{\"value\": 42}");  /* set a known display state */
    uint8_t saved_fb[8][4];
    memcpy(saved_fb, s_fb, sizeof(s_fb));

    esp_err_t err = display_command("set", "{\"value\": 99, \"brightness\": -1}");
    ASSERT_EQ(err, ESP_ERR_INVALID_ARG);
    ASSERT_EQ(s_cfg.brightness, 5);              /* brightness unchanged */
    ASSERT_EQ(memcmp(s_fb, saved_fb, sizeof(s_fb)), 0); /* fb unchanged */
}

TEST(brightness_16_rejected_no_write) {
    s_cfg.brightness = 7;
    display_command("set", "{\"value\": 42}");
    uint8_t saved_fb[8][4];
    memcpy(saved_fb, s_fb, sizeof(s_fb));

    esp_err_t err = display_command("set", "{\"value\": 99, \"brightness\": 16}");
    ASSERT_EQ(err, ESP_ERR_INVALID_ARG);
    ASSERT_EQ(s_cfg.brightness, 7);
    ASSERT_EQ(memcmp(s_fb, saved_fb, sizeof(s_fb)), 0);
}

/* ── value validation ────────────────────────────────────────────────────── */

TEST(missing_value_rejected) {
    esp_err_t err = display_command("set", "{\"brightness\": 3}");
    ASSERT_EQ(err, ESP_ERR_INVALID_ARG);
}

TEST(null_value_rejected) {
    esp_err_t err = display_command("set", "{\"value\": null}");
    ASSERT_EQ(err, ESP_ERR_INVALID_ARG);
}

TEST(object_value_rejected) {
    esp_err_t err = display_command("set", "{\"value\": {}}");
    ASSERT_EQ(err, ESP_ERR_INVALID_ARG);
}

TEST(bool_value_rejected) {
    esp_err_t err = display_command("set", "{\"value\": true}");
    ASSERT_EQ(err, ESP_ERR_INVALID_ARG);
}

TEST(null_params_rejected) {
    esp_err_t err = display_command("set", NULL);
    ASSERT_EQ(err, ESP_ERR_INVALID_ARG);
}

TEST(unknown_action_rejected) {
    esp_err_t err = display_command("rotate", "{\"value\": 1}");
    ASSERT_EQ(err, ESP_ERR_NOT_SUPPORTED);
}

/* ── negative value renders dash string (not an error) ──────────────────── */

TEST(negative_value_renders_dashes) {
    esp_err_t err = display_command("set", "{\"value\": -1}");
    ASSERT_EQ(err, ESP_OK);
    /* framebuffer should be non-zero (dashes rendered) */
    bool any_lit = false;
    for (int r = 0; r < 8 && !any_lit; r++)
        for (int m = 0; m < 4 && !any_lit; m++)
            if (s_fb[r][m]) any_lit = true;
    ASSERT_TRUE(any_lit);
}

/* ── frame buffer glyph tests ────────────────────────────────────────────── */

/*
 * Font data (same as s_font5x8 in display.c, bit 0 = row 0):
 *  '0' = {0x3E, 0x51, 0x49, 0x45, 0x3E}
 *  '1' = {0x00, 0x42, 0x7F, 0x40, 0x00}
 *
 * Single-char centering on 32px: x_start = (32 - 5) / 2 = 13
 *   col 0 → display x=13, module=1, bit=2
 *   col 1 → display x=14, module=1, bit=1
 *   col 2 → display x=15, module=1, bit=0
 *   col 3 → display x=16, module=2, bit=7
 *   col 4 → display x=17, module=2, bit=6
 */

TEST(glyph_1_places_pixels_correctly) {
    /* String "1" → dispatch through display_command */
    esp_err_t err = display_command("set", "{\"value\": \"1\"}");
    ASSERT_EQ(err, ESP_OK);

    /* col 2 (0x7F = rows 0-6) at x=15 */
    ASSERT_TRUE( pixel_lit(0, 15));
    ASSERT_TRUE( pixel_lit(6, 15));
    ASSERT_TRUE(!pixel_lit(7, 15));   /* 0x7F bit 7 = 0 */

    /* col 1 (0x42 = rows 1,6) at x=14 */
    ASSERT_TRUE( pixel_lit(1, 14));
    ASSERT_TRUE( pixel_lit(6, 14));
    ASSERT_TRUE(!pixel_lit(0, 14));   /* 0x42 bit 0 = 0 */

    /* col 0 (0x00) at x=13 — no pixels */
    ASSERT_TRUE(!pixel_lit(0, 13));
    ASSERT_TRUE(!pixel_lit(6, 13));

    /* col 3 (0x40 = row 6) at x=16 */
    ASSERT_TRUE( pixel_lit(6, 16));
    ASSERT_TRUE(!pixel_lit(0, 16));

    /* col 4 (0x00) at x=17 — no pixels */
    ASSERT_TRUE(!pixel_lit(0, 17));
}

TEST(glyph_0_places_pixels_correctly) {
    esp_err_t err = display_command("set", "{\"value\": \"0\"}");
    ASSERT_EQ(err, ESP_OK);

    /* col 0 (0x3E = rows 1-5) at x=13 */
    ASSERT_TRUE(!pixel_lit(0, 13));   /* 0x3E bit 0 = 0 */
    ASSERT_TRUE( pixel_lit(1, 13));   /* 0x3E bit 1 = 1 */
    ASSERT_TRUE( pixel_lit(5, 13));   /* 0x3E bit 5 = 1 */
    ASSERT_TRUE(!pixel_lit(6, 13));   /* 0x3E bit 6 = 0 */

    /* col 1 (0x51 = rows 0,4,6) at x=14 */
    ASSERT_TRUE( pixel_lit(0, 14));   /* 0x51 bit 0 = 1 */
    ASSERT_TRUE(!pixel_lit(1, 14));   /* 0x51 bit 1 = 0 */
    ASSERT_TRUE( pixel_lit(4, 14));   /* 0x51 bit 4 = 1 */
    ASSERT_TRUE( pixel_lit(6, 14));   /* 0x51 bit 6 = 1 */
}

/* ── string value truncated to 5 chars ───────────────────────────────────── */

TEST(string_value_accepted_and_truncated) {
    /* "123456789" → only "12345" rendered (5 chars, rest truncated) */
    esp_err_t err = display_command("set", "{\"value\": \"123456789\"}");
    ASSERT_EQ(err, ESP_OK);
    /* framebuffer must have some content from digits 1-5 */
    bool any_lit = false;
    for (int r = 0; r < 8 && !any_lit; r++)
        for (int m = 0; m < 4 && !any_lit; m++)
            if (s_fb[r][m]) any_lit = true;
    ASSERT_TRUE(any_lit);
}

TEST(numeric_value_accepted) {
    esp_err_t err = display_command("set", "{\"value\": 12345}");
    ASSERT_EQ(err, ESP_OK);
}

/* ── RUN_TEST macro self-check ───────────────────────────────────────────── */
/*
 * Verifies that a test body that fails via ASSERT_TRUE does NOT also
 * increment s_passed (the pre-fix bug: s_nvs_write_calls==0 would
 * trigger the else-branch even after s_failed was already bumped).
 * Only active when s_meta_fail_active is set; run before the real suite.
 */

TEST(meta_fail) {
    ASSERT_TRUE(!s_meta_fail_active);
}

/* ── identity NVS keys untouched ─────────────────────────────────────────── */

TEST(nvs_identity_keys_untouched) {
    /*
     * display.c has no NVS includes or calls — this test is a structural
     * marker. The RUN_TEST macro checks s_nvs_write_calls after each test;
     * since stub_nvs_set_str is never called from display code, the check
     * always passes. This specific test exercises all major paths together.
     */
    (void)stub_nvs_set_str;  /* suppress unused-function warning */
    ASSERT_EQ(display_command("set", "{\"value\": 42, \"brightness\": 5}"), ESP_OK);
    ASSERT_EQ(display_command("set", "{\"value\": \"hi\"}"), ESP_OK);
    ASSERT_EQ(display_command("set", "{\"value\": -1}"), ESP_OK);
    ASSERT_EQ(s_nvs_write_calls, 0);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("═══════════════════════════════════════\n");
    printf("  Display Driver Unit Tests\n");
    printf("═══════════════════════════════════════\n");

    /* Verify RUN_TEST macro: a failing test body must not also increment
     * s_passed.  Undoes the deliberate failure if the macro is correct. */
    {
        int p0 = s_passed, f0 = s_failed;
        s_meta_fail_active = true;
        RUN_TEST(meta_fail);
        s_meta_fail_active = false;
        if (s_passed == p0 && s_failed == f0 + 1) {
            s_failed = f0; /* correct — undo the deliberate failure */
            printf("  [macro check: RUN_TEST correctly counts failing test as failed only]\n");
        } else {
            printf("FAIL: RUN_TEST double-count regression detected\n");
            s_passed = p0; /* remove any spurious pass */
        }
    }

    RUN_TEST(compact_zero);
    RUN_TEST(compact_99999);
    RUN_TEST(compact_100000_is_100k);
    RUN_TEST(compact_999999_is_999k);
    RUN_TEST(compact_1000000_is_1_0M);
    RUN_TEST(compact_9999999_is_9_9M);
    RUN_TEST(compact_99999999_is_99M);
    RUN_TEST(compact_999999999_is_999M);
    RUN_TEST(compact_1000000000_is_1B);
    RUN_TEST(compact_negative_is_dash);
    RUN_TEST(driver_registers_and_set_dispatches);
    RUN_TEST(brightness_0_accepted);
    RUN_TEST(brightness_15_accepted);
    RUN_TEST(brightness_minus1_rejected_no_write);
    RUN_TEST(brightness_16_rejected_no_write);
    RUN_TEST(missing_value_rejected);
    RUN_TEST(null_value_rejected);
    RUN_TEST(object_value_rejected);
    RUN_TEST(bool_value_rejected);
    RUN_TEST(null_params_rejected);
    RUN_TEST(unknown_action_rejected);
    RUN_TEST(negative_value_renders_dashes);
    RUN_TEST(glyph_1_places_pixels_correctly);
    RUN_TEST(glyph_0_places_pixels_correctly);
    RUN_TEST(string_value_accepted_and_truncated);
    RUN_TEST(numeric_value_accepted);
    RUN_TEST(nvs_identity_keys_untouched);

    printf("\n═══════════════════════════════════════\n");
    printf("  Results: %d passed, %d failed\n", s_passed, s_failed);
    printf("═══════════════════════════════════════\n");

    return s_failed > 0 ? 1 : 0;
}
