/**
 * @file rgb_led_matrix.c
 * @brief Driver for Grove RGB LED Matrix 8x8 (STM32F031 + MY9221)
 *
 * I2C register map (verified against Seeed Grove_LED_Matrix_Driver library):
 *   0x04  CMD_SET_DISPLAY_ENABLED   [0x00=off | 0x01=on]
 *   0x05  CMD_SET_DISPLAY_BRIGHTNESS [0-255]
 *   0x10  CMD_SET_COLOR_R            [0-255]
 *   0x11  CMD_SET_COLOR_G            [0-255]
 *   0x12  CMD_SET_COLOR_B            [0-255]
 *   0x20  CMD_SET_ROW_0 .. 0x27 CMD_SET_ROW_7  [0-255 bitmap]
 *   0x28  CMD_DISPLAY_NOW            (trigger refresh, no data byte)
 */

#include "rgb_led_matrix.h"
#include "jettyd_driver.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "drv_matrix";

/* I2C register addresses */
#define REG_DISPLAY_ENABLED  0x04
#define REG_BRIGHTNESS       0x05
#define REG_COLOR_R          0x10
#define REG_COLOR_G          0x11
#define REG_COLOR_B          0x12
#define REG_ROW_BASE         0x20   /* rows 0-7 at 0x20-0x27 */
#define REG_DISPLAY_NOW      0x28

#define MATRIX_ROWS  8
#define I2C_TIMEOUT_MS 100

/* ── State ───────────────────────────────────────────────────────────────── */

static rgb_led_matrix_config_t s_cfg;
static jettyd_driver_t s_driver;

static i2c_master_bus_handle_t s_bus  = NULL;
static i2c_master_dev_handle_t s_dev  = NULL;

static uint8_t  s_pattern[MATRIX_ROWS] = {0};
static uint8_t  s_color_r = 0xFF;
static uint8_t  s_color_g = 0xFF;
static uint8_t  s_color_b = 0xFF;
static bool     s_enabled = false;

/* Current pattern/color as strings for read-back */
static char s_pattern_str[20] = "all_off";
static char s_color_str[8]    = "FFFFFF";

/* ── Named patterns ──────────────────────────────────────────────────────── */

typedef struct { const char *name; uint8_t rows[MATRIX_ROWS]; } named_pattern_t;

static const named_pattern_t NAMED_PATTERNS[] = {
    { "heart",      { 0x00, 0x6C, 0xFE, 0xFE, 0x7C, 0x38, 0x10, 0x00 } },
    { "smiley",     { 0x3C, 0x42, 0xA5, 0x81, 0xA5, 0x99, 0x42, 0x3C } },
    { "sad",        { 0x3C, 0x42, 0xA5, 0x81, 0x99, 0xA5, 0x42, 0x3C } },
    { "check",      { 0x01, 0x03, 0x06, 0x8C, 0xD8, 0x70, 0x20, 0x00 } },
    { "x",          { 0x81, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x81 } },
    { "arrow_up",   { 0x18, 0x3C, 0x7E, 0xFF, 0x18, 0x18, 0x18, 0x00 } },
    { "arrow_down", { 0x00, 0x18, 0x18, 0x18, 0xFF, 0x7E, 0x3C, 0x18 } },
    { "arrow_left", { 0x10, 0x30, 0x70, 0xFF, 0xFF, 0x70, 0x30, 0x10 } },
    { "arrow_right",{ 0x08, 0x0C, 0x0E, 0xFF, 0xFF, 0x0E, 0x0C, 0x08 } },
    { "all_on",     { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF } },
    { "all_off",    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
    { "border",     { 0xFF, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0xFF } },
    { "diamond",    { 0x18, 0x3C, 0x7E, 0xFF, 0xFF, 0x7E, 0x3C, 0x18 } },
    { "exclaim",    { 0x18, 0x18, 0x18, 0x18, 0x00, 0x00, 0x18, 0x00 } },
    { "question",   { 0x3C, 0x42, 0x02, 0x0C, 0x10, 0x00, 0x10, 0x00 } },
};
#define NAMED_PATTERN_COUNT (sizeof(NAMED_PATTERNS) / sizeof(NAMED_PATTERNS[0]))

/* ── I2C helpers ─────────────────────────────────────────────────────────── */

static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, 2, I2C_TIMEOUT_MS);
}

static esp_err_t write_reg_burst(uint8_t reg_start, const uint8_t *data, size_t len)
{
    /* Send reg + all data bytes in a single I2C transaction */
    uint8_t buf[MATRIX_ROWS + 1];
    buf[0] = reg_start;
    memcpy(&buf[1], data, len);
    return i2c_master_transmit(s_dev, buf, len + 1, I2C_TIMEOUT_MS);
}

static esp_err_t trigger_display(void)
{
    uint8_t buf[1] = { REG_DISPLAY_NOW };
    return i2c_master_transmit(s_dev, buf, 1, I2C_TIMEOUT_MS);
}

/* ── Display helpers ─────────────────────────────────────────────────────── */

static esp_err_t push_color(uint8_t r, uint8_t g, uint8_t b)
{
    esp_err_t err;
    if ((err = write_reg(REG_COLOR_R, r)) != ESP_OK) return err;
    if ((err = write_reg(REG_COLOR_G, g)) != ESP_OK) return err;
    if ((err = write_reg(REG_COLOR_B, b)) != ESP_OK) return err;
    return ESP_OK;
}

static esp_err_t push_pattern(const uint8_t rows[MATRIX_ROWS])
{
    return write_reg_burst(REG_ROW_BASE, rows, MATRIX_ROWS);
}

static esp_err_t display_now(void)
{
    esp_err_t err;
    if ((err = push_color(s_color_r, s_color_g, s_color_b)) != ESP_OK) return err;
    if ((err = push_pattern(s_pattern))                      != ESP_OK) return err;
    if ((err = trigger_display())                             != ESP_OK) return err;
    return ESP_OK;
}

/* ── Pattern parsing ─────────────────────────────────────────────────────── */

static bool parse_named_pattern(const char *name, uint8_t out[MATRIX_ROWS])
{
    for (size_t i = 0; i < NAMED_PATTERN_COUNT; i++) {
        if (strcmp(NAMED_PATTERNS[i].name, name) == 0) {
            memcpy(out, NAMED_PATTERNS[i].rows, MATRIX_ROWS);
            return true;
        }
    }
    return false;
}

static bool parse_hex_pattern(const char *hex, uint8_t out[MATRIX_ROWS])
{
    /* Expect exactly 16 hex chars = 8 bytes */
    if (strlen(hex) != 16) return false;
    for (int i = 0; i < MATRIX_ROWS; i++) {
        char byte_str[3] = { hex[i * 2], hex[i * 2 + 1], '\0' };
        char *end;
        long val = strtol(byte_str, &end, 16);
        if (end != byte_str + 2) return false;
        out[i] = (uint8_t)val;
    }
    return true;
}

static bool parse_color(const char *hex, uint8_t *r, uint8_t *g, uint8_t *b)
{
    /* Expect exactly 6 hex chars e.g. FF0000 */
    if (strlen(hex) != 6) return false;
    char buf[3];
    char *end;
    buf[0] = hex[0]; buf[1] = hex[1]; buf[2] = '\0';
    *r = (uint8_t)strtol(buf, &end, 16); if (end != buf + 2) return false;
    buf[0] = hex[2]; buf[1] = hex[3];
    *g = (uint8_t)strtol(buf, &end, 16); if (end != buf + 2) return false;
    buf[0] = hex[4]; buf[1] = hex[5];
    *b = (uint8_t)strtol(buf, &end, 16); if (end != buf + 2) return false;
    return true;
}

/* ── Driver operations ───────────────────────────────────────────────────── */

static esp_err_t matrix_init(const void *config)
{
    const rgb_led_matrix_config_t *c = (const rgb_led_matrix_config_t *)config;
    s_cfg = *c;
    if (s_cfg.i2c_addr == 0) s_cfg.i2c_addr = 0x65;

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port            = s_cfg.i2c_port,
        .sda_io_num          = s_cfg.sda_pin,
        .scl_io_num          = s_cfg.scl_pin,
        .clk_source          = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt   = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = s_cfg.i2c_addr,
        .scl_speed_hz    = 100000,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C device add failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Start with display off, white colour */
    write_reg(REG_DISPLAY_ENABLED, 0x00);
    ESP_LOGI(TAG, "RGB LED Matrix init: sda=%d scl=%d addr=0x%02X",
             s_cfg.sda_pin, s_cfg.scl_pin, s_cfg.i2c_addr);
    return ESP_OK;
}

static esp_err_t matrix_deinit(void)
{
    write_reg(REG_DISPLAY_ENABLED, 0x00);
    if (s_dev) { i2c_master_bus_rm_device(s_dev); s_dev = NULL; }
    if (s_bus) { i2c_del_master_bus(s_bus); s_bus = NULL; }
    return ESP_OK;
}

static esp_err_t matrix_switch_on(uint32_t duration_ms)
{
    (void)duration_ms; /* Matrix has no auto-off; stays until switch_off or new pattern */
    esp_err_t err = display_now();
    if (err != ESP_OK) return err;
    err = write_reg(REG_DISPLAY_ENABLED, 0x01);
    if (err == ESP_OK) s_enabled = true;
    return err;
}

static esp_err_t matrix_switch_off(void)
{
    esp_err_t err = write_reg(REG_DISPLAY_ENABLED, 0x00);
    if (err == ESP_OK) {
        s_enabled = false;
        strlcpy(s_pattern_str, "all_off", sizeof(s_pattern_str));
    }
    return err;
}

static bool matrix_get_state(void)
{
    return s_enabled;
}

static jettyd_value_t matrix_read(const char *capability)
{
    jettyd_value_t val = {0};
    if (strcmp(capability, "pattern") == 0) {
        val.type = JETTYD_VAL_STRING;
        strlcpy(val.str_val, s_pattern_str, sizeof(val.str_val));
        val.valid = true;
    } else if (strcmp(capability, "color") == 0) {
        val.type = JETTYD_VAL_STRING;
        strlcpy(val.str_val, s_color_str, sizeof(val.str_val));
        val.valid = true;
    }
    return val;
}

static esp_err_t matrix_write(const char *capability, jettyd_value_t value)
{
    if (value.type != JETTYD_VAL_STRING) return ESP_ERR_INVALID_ARG;

    if (strcmp(capability, "pattern") == 0) {
        uint8_t rows[MATRIX_ROWS];
        bool ok = parse_named_pattern(value.str_val, rows)
               || parse_hex_pattern(value.str_val, rows);
        if (!ok) {
            ESP_LOGW(TAG, "Unknown pattern: '%s'", value.str_val);
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(s_pattern, rows, MATRIX_ROWS);
        strlcpy(s_pattern_str, value.str_val, sizeof(s_pattern_str));
        ESP_LOGI(TAG, "Pattern set: %s", s_pattern_str);
        if (s_enabled) return display_now();
        return ESP_OK;

    } else if (strcmp(capability, "color") == 0) {
        uint8_t r, g, b;
        if (!parse_color(value.str_val, &r, &g, &b)) {
            ESP_LOGW(TAG, "Invalid color: '%s' (want RRGGBB hex)", value.str_val);
            return ESP_ERR_INVALID_ARG;
        }
        s_color_r = r; s_color_g = g; s_color_b = b;
        strlcpy(s_color_str, value.str_val, sizeof(s_color_str));
        ESP_LOGI(TAG, "Color set: #%s", s_color_str);
        if (s_enabled) return display_now();
        return ESP_OK;
    }

    return ESP_ERR_INVALID_ARG;
}

static esp_err_t matrix_self_test(void)
{
    /* Flash all LEDs white briefly */
    uint8_t all_on[MATRIX_ROWS];
    memset(all_on, 0xFF, MATRIX_ROWS);
    push_color(0xFF, 0xFF, 0xFF);
    push_pattern(all_on);
    trigger_display();
    write_reg(REG_DISPLAY_ENABLED, 0x01);
    vTaskDelay(pdMS_TO_TICKS(200));
    write_reg(REG_DISPLAY_ENABLED, 0x00);
    return ESP_OK;
}

/* ── Registration ────────────────────────────────────────────────────────── */

void rgb_led_matrix_register(const char *instance, const void *config)
{
    matrix_init(config);

    memset(&s_driver, 0, sizeof(s_driver));
    strlcpy(s_driver.instance, instance, JETTYD_MAX_INSTANCE_NAME);
    strlcpy(s_driver.driver_name, "rgb_led_matrix", sizeof(s_driver.driver_name));

    s_driver.capability_count = 2;

    strlcpy(s_driver.capabilities[0].name, "pattern", sizeof(s_driver.capabilities[0].name));
    s_driver.capabilities[0].type       = JETTYD_CAP_WRITABLE;
    s_driver.capabilities[0].value_type = JETTYD_VAL_STRING;

    strlcpy(s_driver.capabilities[1].name, "color", sizeof(s_driver.capabilities[1].name));
    s_driver.capabilities[1].type       = JETTYD_CAP_WRITABLE;
    s_driver.capabilities[1].value_type = JETTYD_VAL_STRING;

    s_driver.init       = matrix_init;
    s_driver.deinit     = matrix_deinit;
    s_driver.read       = matrix_read;
    s_driver.write      = matrix_write;
    s_driver.switch_on  = matrix_switch_on;
    s_driver.switch_off = matrix_switch_off;
    s_driver.get_state  = matrix_get_state;
    s_driver.self_test  = matrix_self_test;

    JETTYD_REGISTER_DRIVER(&s_driver);
}
