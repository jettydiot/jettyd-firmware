/**
 * @file rgb_led_matrix.c
 * @brief Driver for Grove RGB LED Matrix 8x8 (STM32F031 + MY9221)
 *
 * Uses the command-based I2C protocol from the Seeed Grove_Two_RGB_LED_Matrix
 * library. Named patterns map to built-in emoji (their colors are fixed in
 * the STM32F031 firmware). Hex bitmap patterns use displayFrames with the
 * nearest available palette color to the requested RGB.
 */

#include "rgb_led_matrix.h"
#include "jettyd_driver.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

static const char *TAG = "drv_matrix";

/* Command bytes — Seeed Grove_Two_RGB_LED_Matrix protocol */
#define CMD_DISP_EMOJI       0x02
#define CMD_DISP_CUSTOM      0x05
#define CMD_DISP_OFF         0x06
#define CMD_DISP_COLOR_BLOCK 0x0d
#define CMD_CONTINUE_DATA    0x81  /* prepended to continuation I2C transactions */

#define MATRIX_ROWS    8
#define I2C_TIMEOUT_MS 100

/* ── State ───────────────────────────────────────────────────────────────── */

static rgb_led_matrix_config_t  s_cfg;
static jettyd_driver_t          s_driver;
static i2c_master_bus_handle_t  s_bus    = NULL;
static i2c_master_dev_handle_t  s_dev    = NULL;

static char s_pattern_str[20] = "";
static char s_color_str[8]    = "FFFFFF";
static bool s_enabled         = false;

/* ── Named emoji patterns ────────────────────────────────────────────────── */
/* Indices come from the Seeed library header (0-34). */

typedef struct { const char *name; uint8_t index; } emoji_entry_t;

static const emoji_entry_t EMOJI_TABLE[] = {
    { "smile",        0  },
    { "smiley",       0  },
    { "laugh",        1  },
    { "sad",          2  },
    { "mad",          3  },
    { "angry",        4  },
    { "cry",          5  },
    { "greedy",       6  },
    { "cool",         7  },
    { "shy",          8  },
    { "awkward",      9  },
    { "heart",        10 },
    { "small_heart",  11 },
    { "broken_heart", 12 },
    { "waterdrop",    13 },
    { "flame",        14 },
    { "fire",         14 },
    { "creeper",      15 },
    { "sword",        17 },
    { "house",        20 },
    { "tree",         21 },
    { "flower",       22 },
    { "umbrella",     23 },
    { "rain",         24 },
    { "monster",      25 },
    { "crab",         26 },
    { "duck",         27 },
    { "rabbit",       28 },
    { "cat",          29 },
    { "up",           30 },
    { "arrow_up",     30 },
    { "down",         31 },
    { "arrow_down",   31 },
    { "left",         32 },
    { "arrow_left",   32 },
    { "right",        33 },
    { "arrow_right",  33 },
};
#define EMOJI_COUNT (sizeof(EMOJI_TABLE) / sizeof(EMOJI_TABLE[0]))

/* ── Bitmap-only patterns (no emoji equivalent) ──────────────────────────── */
/* Displayed via displayFrames with the nearest palette colour. */

typedef struct { const char *name; uint8_t rows[MATRIX_ROWS]; } bitmap_entry_t;

static const bitmap_entry_t BITMAP_TABLE[] = {
    { "check",    { 0x01, 0x03, 0x06, 0x8C, 0xD8, 0x70, 0x20, 0x00 } },
    { "x",        { 0x81, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x81 } },
    { "all_on",   { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF } },
    { "border",   { 0xFF, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0xFF } },
    { "diamond",  { 0x18, 0x3C, 0x7E, 0xFF, 0xFF, 0x7E, 0x3C, 0x18 } },
    { "exclaim",  { 0x18, 0x18, 0x18, 0x18, 0x00, 0x00, 0x18, 0x00 } },
    { "question", { 0x3C, 0x42, 0x02, 0x0C, 0x10, 0x00, 0x10, 0x00 } },
};
#define BITMAP_COUNT (sizeof(BITMAP_TABLE) / sizeof(BITMAP_TABLE[0]))

/* ── Palette colour matching ─────────────────────────────────────────────── */
/* The STM32F031 uses a fixed 8-bit palette; 0xFF = off. */

static const struct { uint8_t c, r, g, b; } PALETTE[] = {
    { 0x00, 255,   0,   0 }, /* red    */
    { 0x12, 255, 140,   0 }, /* orange */
    { 0x18, 255, 255,   0 }, /* yellow */
    { 0x52,   0, 255,   0 }, /* green  */
    { 0x7f,   0, 255, 255 }, /* cyan   */
    { 0xaa,   0,   0, 255 }, /* blue   */
    { 0xc3, 128,   0, 255 }, /* purple */
    { 0xdc, 255,   0, 128 }, /* pink   */
    { 0xfe, 255, 255, 255 }, /* white  */
};
#define PALETTE_COUNT (sizeof(PALETTE) / sizeof(PALETTE[0]))

static uint8_t nearest_palette_color(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t best = PALETTE[0].c;
    int32_t best_dist = INT32_MAX;
    for (size_t i = 0; i < PALETTE_COUNT; i++) {
        int32_t dr = (int32_t)r - PALETTE[i].r;
        int32_t dg = (int32_t)g - PALETTE[i].g;
        int32_t db = (int32_t)b - PALETTE[i].b;
        int32_t dist = dr*dr + dg*dg + db*db;
        if (dist < best_dist) { best_dist = dist; best = PALETTE[i].c; }
    }
    return best;
}

/* ── I2C helper ──────────────────────────────────────────────────────────── */

static esp_err_t i2c_send(const uint8_t *data, size_t len)
{
    if (!s_dev) {
        ESP_LOGE(TAG, "i2c_send: device handle is NULL (init failed?)");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = i2c_master_transmit(s_dev, data, len, I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_send cmd=0x%02X len=%u: %s", data[0], (unsigned)len, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "i2c_send cmd=0x%02X len=%u OK", data[0], (unsigned)len);
    }
    return err;
}

/* ── Display commands ────────────────────────────────────────────────────── */

static esp_err_t cmd_display_emoji(uint8_t index)
{
    uint8_t buf[5] = {
        CMD_DISP_EMOJI,
        index,
        0x00, 0x00, /* duration (ignored when forever=1) */
        0x01,       /* forever */
    };
    return i2c_send(buf, sizeof(buf));
}

static esp_err_t cmd_display_frames(const uint8_t bitmap[MATRIX_ROWS], uint8_t palette_color)
{
    /*
     * 72-byte frame buffer layout:
     *   [0]    CMD_DISP_CUSTOM (0x05)
     *   [1..2] duration lo/hi (0 — ignored because forever=1)
     *   [3]    forever (1)
     *   [4]    frame_count (1)
     *   [5]    frame_index (0)
     *   [6..7] reserved
     *   [8..71] 64 pixel bytes — row-major, MSB = leftmost pixel
     *           on-pixel = palette_color, off-pixel = 0xFF
     *
     * Sent as three I2C transactions (matching Arduino library):
     *   tx1: data[0..23]       (24 bytes)
     *   tx2: 0x81 + data[24..47] (25 bytes)
     *   tx3: 0x81 + data[48..71] (25 bytes)
     */
    uint8_t data[72] = {0};
    data[0] = CMD_DISP_CUSTOM;
    data[3] = 0x01; /* forever */
    data[4] = 0x01; /* 1 frame */

    for (int row = 0; row < MATRIX_ROWS; row++) {
        for (int col = 0; col < 8; col++) {
            bool on = (bitmap[row] >> (7 - col)) & 1;
            data[8 + row * 8 + col] = on ? palette_color : 0xFF;
        }
    }

    esp_err_t err;
    uint8_t cont[25];

    err = i2c_send(data, 24);
    if (err != ESP_OK) { ESP_LOGE(TAG, "frames tx1: %s", esp_err_to_name(err)); return err; }
    vTaskDelay(pdMS_TO_TICKS(1));

    cont[0] = CMD_CONTINUE_DATA;
    memcpy(cont + 1, data + 24, 24);
    err = i2c_send(cont, 25);
    if (err != ESP_OK) { ESP_LOGE(TAG, "frames tx2: %s", esp_err_to_name(err)); return err; }
    vTaskDelay(pdMS_TO_TICKS(1));

    memcpy(cont + 1, data + 48, 24);
    err = i2c_send(cont, 25);
    if (err != ESP_OK) ESP_LOGE(TAG, "frames tx3: %s", esp_err_to_name(err));
    return err;
}

static esp_err_t cmd_display_color_block(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t buf[7] = {
        CMD_DISP_COLOR_BLOCK,
        r, g, b,
        0x00, 0x00, /* duration (ignored) */
        0x01,       /* forever */
    };
    return i2c_send(buf, sizeof(buf));
}

static esp_err_t cmd_display_off(void)
{
    uint8_t buf[1] = { CMD_DISP_OFF };
    return i2c_send(buf, sizeof(buf));
}

/* ── Parsing helpers ─────────────────────────────────────────────────────── */

static bool parse_color(const char *hex, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (strlen(hex) != 6) return false;
    char buf[3]; char *end;
    buf[0] = hex[0]; buf[1] = hex[1]; buf[2] = '\0';
    *r = (uint8_t)strtol(buf, &end, 16); if (end != buf+2) return false;
    buf[0] = hex[2]; buf[1] = hex[3];
    *g = (uint8_t)strtol(buf, &end, 16); if (end != buf+2) return false;
    buf[0] = hex[4]; buf[1] = hex[5];
    *b = (uint8_t)strtol(buf, &end, 16); if (end != buf+2) return false;
    return true;
}

static bool parse_hex_bitmap(const char *hex, uint8_t out[MATRIX_ROWS])
{
    if (strlen(hex) != 16) return false;
    for (int i = 0; i < MATRIX_ROWS; i++) {
        char byte_str[3] = { hex[i*2], hex[i*2+1], '\0' };
        char *end;
        long val = strtol(byte_str, &end, 16);
        if (end != byte_str+2) return false;
        out[i] = (uint8_t)val;
    }
    return true;
}

/* ── Core display dispatch ───────────────────────────────────────────────── */

static esp_err_t do_display(void)
{
    uint8_t r = 0xFF, g = 0xFF, b = 0xFF;
    parse_color(s_color_str, &r, &g, &b);

    /* 1. Emoji table (named patterns with fixed built-in colors) */
    for (size_t i = 0; i < EMOJI_COUNT; i++) {
        if (strcmp(EMOJI_TABLE[i].name, s_pattern_str) == 0) {
            ESP_LOGI(TAG, "emoji '%s' idx=%d", s_pattern_str, EMOJI_TABLE[i].index);
            return cmd_display_emoji(EMOJI_TABLE[i].index);
        }
    }

    /* 2. Bitmap table (custom shapes — respects colour param) */
    for (size_t i = 0; i < BITMAP_COUNT; i++) {
        if (strcmp(BITMAP_TABLE[i].name, s_pattern_str) == 0) {
            uint8_t col = nearest_palette_color(r, g, b);
            ESP_LOGI(TAG, "bitmap '%s' color=#%s palette=0x%02X", s_pattern_str, s_color_str, col);
            return cmd_display_frames(BITMAP_TABLE[i].rows, col);
        }
    }

    /* 3. 16-char hex bitmap string */
    uint8_t bitmap[MATRIX_ROWS];
    if (parse_hex_bitmap(s_pattern_str, bitmap)) {
        uint8_t col = nearest_palette_color(r, g, b);
        ESP_LOGI(TAG, "hex bitmap color=#%s palette=0x%02X", s_color_str, col);
        return cmd_display_frames(bitmap, col);
    }

    /* 4. No pattern — solid colour block */
    ESP_LOGI(TAG, "color block #%s", s_color_str);
    return cmd_display_color_block(r, g, b);
}

/* ── Driver operations ───────────────────────────────────────────────────── */

static esp_err_t matrix_init(const void *config)
{
    const rgb_led_matrix_config_t *c = (const rgb_led_matrix_config_t *)config;
    s_cfg = *c;
    if (s_cfg.i2c_addr == 0) s_cfg.i2c_addr = 0x65;

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = s_cfg.i2c_port,
        .sda_io_num        = s_cfg.sda_pin,
        .scl_io_num        = s_cfg.scl_pin,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) { ESP_LOGE(TAG, "I2C bus: %s", esp_err_to_name(err)); return err; }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = s_cfg.i2c_addr,
        .scl_speed_hz    = 100000,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) { ESP_LOGE(TAG, "I2C dev: %s", esp_err_to_name(err)); return err; }

    /* Probe the device — confirms wiring + address before any display command */
    esp_err_t probe = i2c_master_probe(s_bus, s_cfg.i2c_addr, I2C_TIMEOUT_MS);
    if (probe != ESP_OK) {
        ESP_LOGE(TAG, "I2C probe addr=0x%02X FAILED: %s — check wiring & address",
                 s_cfg.i2c_addr, esp_err_to_name(probe));
    } else {
        ESP_LOGI(TAG, "I2C probe addr=0x%02X OK", s_cfg.i2c_addr);
    }

    cmd_display_off();
    ESP_LOGI(TAG, "RGB LED Matrix init: sda=%d scl=%d addr=0x%02X",
             s_cfg.sda_pin, s_cfg.scl_pin, s_cfg.i2c_addr);
    return ESP_OK;
}

static esp_err_t matrix_deinit(void)
{
    cmd_display_off();
    if (s_dev) { i2c_master_bus_rm_device(s_dev); s_dev = NULL; }
    if (s_bus) { i2c_del_master_bus(s_bus); s_bus = NULL; }
    return ESP_OK;
}

static esp_err_t matrix_switch_on(uint32_t duration_ms)
{
    (void)duration_ms;
    ESP_LOGI(TAG, "switch_on: pattern='%s' color='%s'", s_pattern_str, s_color_str);
    esp_err_t err = do_display();
    ESP_LOGI(TAG, "switch_on result: %s", esp_err_to_name(err));
    if (err == ESP_OK) s_enabled = true;
    return err;
}

static esp_err_t matrix_switch_off(void)
{
    ESP_LOGI(TAG, "switch_off called");
    esp_err_t err = cmd_display_off();
    ESP_LOGI(TAG, "switch_off result: %s", esp_err_to_name(err));
    if (err == ESP_OK) s_enabled = false;
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
        strlcpy(s_pattern_str, value.str_val, sizeof(s_pattern_str));
        ESP_LOGI(TAG, "pattern: %s", s_pattern_str);
        if (s_enabled) return do_display();
        return ESP_OK;
    } else if (strcmp(capability, "color") == 0) {
        uint8_t r, g, b;
        if (!parse_color(value.str_val, &r, &g, &b)) {
            ESP_LOGW(TAG, "invalid color: '%s'", value.str_val);
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(s_color_str, value.str_val, sizeof(s_color_str));
        ESP_LOGI(TAG, "color: #%s", s_color_str);
        if (s_enabled) return do_display();
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t matrix_self_test(void)
{
    /* Show the heart emoji for 1 second then turn off */
    uint8_t buf[5] = { CMD_DISP_EMOJI, 10, 0xE8, 0x03, 0x00 }; /* 1000ms, not forever */
    i2c_send(buf, sizeof(buf));
    vTaskDelay(pdMS_TO_TICKS(1100));
    return cmd_display_off();
}

/* ── Registration ────────────────────────────────────────────────────────── */

static esp_err_t matrix_mcp_show(const char *params_json, char *out, size_t out_len)
{
    char pattern[32] = "solid";
    char color[16]   = "white";

    if (params_json) {
        const char *p = strstr(params_json, "\"pattern\":\"");
        if (p) {
            p += 11;
            size_t i = 0;
            while (*p && *p != '"' && i < sizeof(pattern) - 1) pattern[i++] = *p++;
            pattern[i] = '\0';
        }
        p = strstr(params_json, "\"color\":\"");
        if (p) {
            p += 9;
            size_t i = 0;
            while (*p && *p != '"' && i < sizeof(color) - 1) color[i++] = *p++;
            color[i] = '\0';
        }
    }

    jettyd_value_t pv = {.type = JETTYD_VAL_STRING, .valid = true};
    strlcpy(pv.str_val, pattern, sizeof(pv.str_val));
    matrix_write("pattern", pv);

    jettyd_value_t cv = {.type = JETTYD_VAL_STRING, .valid = true};
    strlcpy(cv.str_val, color, sizeof(cv.str_val));
    matrix_write("color", cv);

    esp_err_t err = matrix_switch_on(0);
    if (err == ESP_OK) snprintf(out, out_len, "{\"state\":true}");
    return err;
}

static esp_err_t matrix_mcp_clear(const char *params_json, char *out, size_t out_len)
{
    (void)params_json;
    esp_err_t err = matrix_switch_off();
    if (err == ESP_OK) snprintf(out, out_len, "{\"state\":false}");
    return err;
}

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

    static const char *schema_show = "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"},\"color\":{\"type\":\"string\"}}}";
    static const char *schema_none = "{\"type\":\"object\",\"properties\":{}}";

    strlcpy(s_driver.mcp_tools[0].name, "matrix_show", sizeof(s_driver.mcp_tools[0].name));
    s_driver.mcp_tools[0].description = "Display a pattern on the LED matrix";
    s_driver.mcp_tools[0].input_schema_json = schema_show;
    s_driver.mcp_tools[0].handler = matrix_mcp_show;

    strlcpy(s_driver.mcp_tools[1].name, "matrix_clear", sizeof(s_driver.mcp_tools[1].name));
    s_driver.mcp_tools[1].description = "Turn off the LED matrix";
    s_driver.mcp_tools[1].input_schema_json = schema_none;
    s_driver.mcp_tools[1].handler = matrix_mcp_clear;

    s_driver.mcp_tool_count = 2;

    JETTYD_REGISTER_DRIVER(&s_driver);
}
