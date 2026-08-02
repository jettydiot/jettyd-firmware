/**
 * @file display.c
 * @brief MAX7219 LED matrix display driver — bit-bang SPI, FC16 4-module layout.
 *
 * Commands accepted (via MQTT command topic):
 *   {"action": "display.set", "params": {"value": <string|number>, "brightness": <0-15>}}
 *     Renders the value on the 32x8 pixel display.  Numeric values are
 *     compacted to fit ~5 characters; strings are truncated at 5 chars.
 *     Negative numbers render as "----".  Optional brightness updates the
 *     MAX7219 intensity register atomically with the frame latch.
 *
 * Hardware:
 *   - 4 MAX7219 ICs daisy-chained in FC16-style layout (32x8 usable pixels)
 *   - Bit-bang SPI over three GPIO pins (DIN, CLK, CS)
 *   - Module 0 = leftmost visual position (last in SPI chain from MCU)
 */

#include "display.h"
#include "jettyd_driver.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "drv_display";

/* ── Static state ────────────────────────────────────────────────────────── */

static display_config_t s_cfg;
static jettyd_driver_t  s_driver;

/* Frame buffer: [row 0..7][module 0..3], module 0 = leftmost display.
 * Each byte: bit 7 = leftmost column of that module. */
static uint8_t s_fb[8][4];

#define DISPLAY_MAX_MODULES 4

/* ── MAX7219 register addresses ──────────────────────────────────────────── */

#define MAX7219_REG_NOOP        0x00
#define MAX7219_REG_DIGIT0      0x01
#define MAX7219_REG_DECODE      0x09
#define MAX7219_REG_INTENSITY   0x0A
#define MAX7219_REG_SCANLIMIT   0x0B
#define MAX7219_REG_SHUTDOWN    0x0C
#define MAX7219_REG_DISPLAYTEST 0x0F

/* ── 5×8 ASCII font (column-major, bit 0 = row 0 / top) ─────────────────── */

static const uint8_t s_font5x8[][5] = {
    /* '0' */ {0x3E, 0x51, 0x49, 0x45, 0x3E},
    /* '1' */ {0x00, 0x42, 0x7F, 0x40, 0x00},
    /* '2' */ {0x72, 0x49, 0x49, 0x49, 0x46},
    /* '3' */ {0x21, 0x41, 0x4D, 0x4B, 0x31},
    /* '4' */ {0x18, 0x14, 0x12, 0x7F, 0x10},
    /* '5' */ {0x27, 0x45, 0x45, 0x45, 0x39},
    /* '6' */ {0x3C, 0x4A, 0x49, 0x49, 0x30},
    /* '7' */ {0x01, 0x71, 0x09, 0x05, 0x03},
    /* '8' */ {0x36, 0x49, 0x49, 0x49, 0x36},
    /* '9' */ {0x06, 0x49, 0x49, 0x29, 0x1E},
    /* '-' */ {0x08, 0x08, 0x08, 0x08, 0x08},
    /* '.' */ {0x00, 0x60, 0x60, 0x00, 0x00},
    /* 'k' */ {0x7F, 0x10, 0x28, 0x44, 0x00},
    /* 'M' */ {0x7F, 0x02, 0x04, 0x02, 0x7F},
    /* 'B' */ {0x7F, 0x49, 0x49, 0x49, 0x36},
    /* ' ' */ {0x00, 0x00, 0x00, 0x00, 0x00},
};

static const char s_font_chars[] = "0123456789-.kMB ";

static const uint8_t *font_glyph(char c)
{
    for (int i = 0; s_font_chars[i] != '\0'; i++) {
        if (s_font_chars[i] == c) return s_font5x8[i];
    }
    return s_font5x8[15]; /* ' ' — blank for unsupported chars */
}

/* ── Number compaction ───────────────────────────────────────────────────── */

static void compact_number(long long n, char *out, size_t out_len)
{
    if (n < 0LL) {
        strlcpy(out, "----", out_len);
        return;
    }
    if (n < 100000LL) {
        snprintf(out, out_len, "%lld", n);
    } else if (n < 1000000LL) {
        snprintf(out, out_len, "%lldk", n / 1000LL);
    } else if (n < 10000000LL) {
        long long maj = n / 1000000LL;
        long long min = (n % 1000000LL) / 100000LL;
        snprintf(out, out_len, "%lld.%lldM", maj, min);
    } else if (n < 1000000000LL) {
        snprintf(out, out_len, "%lldM", n / 1000000LL);
    } else {
        snprintf(out, out_len, "%lldB", n / 1000000000LL);
    }
}

/* ── Frame buffer rendering ──────────────────────────────────────────────── */

static void display_render(const char *text)
{
    memset(s_fb, 0, sizeof(s_fb));

    size_t len = strlen(text);
    if (len > 5) len = 5;
    if (len == 0) return;

    /* Centre text: each char is 5px wide + 1px gap, except last char (no gap) */
    int pixel_width = (int)(len * 6) - 1;
    int x_start = (32 - pixel_width) / 2;
    if (x_start < 0) x_start = 0;

    for (size_t i = 0; i < len; i++) {
        int x = x_start + (int)(i * 6);
        const uint8_t *glyph = font_glyph(text[i]);
        for (int col = 0; col < 5; col++) {
            int display_x = x + col;
            if (display_x < 0 || display_x >= 32) continue;
            int module  = display_x / 8;
            int bit_pos = 7 - (display_x % 8);
            uint8_t glyph_col = glyph[col];
            for (int row = 0; row < 8; row++) {
                if (glyph_col & (1u << row)) {
                    s_fb[row][module] |= (uint8_t)(1u << bit_pos);
                }
            }
        }
    }
}

/* ── SPI bit-bang ────────────────────────────────────────────────────────── */

static void spi_send_byte(uint8_t byte)
{
    for (int i = 7; i >= 0; i--) {
        gpio_set_level((gpio_num_t)s_cfg.pin_din, (byte >> i) & 1u);
        gpio_set_level((gpio_num_t)s_cfg.pin_clk, 1);
        gpio_set_level((gpio_num_t)s_cfg.pin_clk, 0);
    }
}

/* Write register `reg` with value `data` to all chained modules. */
static void max7219_write_reg(uint8_t reg, uint8_t data)
{
    uint8_t mods = s_cfg.num_modules > DISPLAY_MAX_MODULES
                   ? DISPLAY_MAX_MODULES : s_cfg.num_modules;
    gpio_set_level((gpio_num_t)s_cfg.pin_cs, 0);
    for (uint8_t m = 0; m < mods; m++) {
        spi_send_byte(reg);
        spi_send_byte(data);
    }
    gpio_set_level((gpio_num_t)s_cfg.pin_cs, 1);
}

/* Push the full frame buffer to the display.
 * For row r, module 0 (leftmost) is the last chip in the SPI chain — it
 * receives the last 16 bits we send, so we iterate modules in reverse order. */
static void max7219_refresh(void)
{
    uint8_t mods = s_cfg.num_modules > DISPLAY_MAX_MODULES
                   ? DISPLAY_MAX_MODULES : s_cfg.num_modules;
    for (int row = 0; row < 8; row++) {
        gpio_set_level((gpio_num_t)s_cfg.pin_cs, 0);
        for (int m = (int)mods - 1; m >= 0; m--) {
            spi_send_byte((uint8_t)(row + 1)); /* MAX7219 digit reg 1-8 */
            spi_send_byte(s_fb[row][m]);
        }
        gpio_set_level((gpio_num_t)s_cfg.pin_cs, 1);
    }
}

/* ── Driver ops ──────────────────────────────────────────────────────────── */

static esp_err_t display_init(const void *config)
{
    const display_config_t *c = (const display_config_t *)config;
    s_cfg = *c;
    if (s_cfg.num_modules == 0) s_cfg.num_modules = 4;

    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << s_cfg.pin_din)
                      | (1ULL << s_cfg.pin_clk)
                      | (1ULL << s_cfg.pin_cs),
        .mode             = GPIO_MODE_OUTPUT,
        .pull_up_en       = GPIO_PULLUP_DISABLE,
        .pull_down_en     = GPIO_PULLDOWN_DISABLE,
        .intr_type        = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_cfg);
    gpio_set_level((gpio_num_t)s_cfg.pin_cs, 1);

    max7219_write_reg(MAX7219_REG_SHUTDOWN,    0x01);
    max7219_write_reg(MAX7219_REG_DECODE,      0x00);
    max7219_write_reg(MAX7219_REG_SCANLIMIT,   0x07);
    max7219_write_reg(MAX7219_REG_DISPLAYTEST, 0x00);
    max7219_write_reg(MAX7219_REG_INTENSITY,   s_cfg.brightness);

    memset(s_fb, 0, sizeof(s_fb));
    max7219_refresh();

    ESP_LOGI(TAG, "Display init: DIN=%d CLK=%d CS=%d modules=%d brightness=%d",
             s_cfg.pin_din, s_cfg.pin_clk, s_cfg.pin_cs,
             s_cfg.num_modules, s_cfg.brightness);
    return ESP_OK;
}

static esp_err_t display_deinit(void)
{
    max7219_write_reg(MAX7219_REG_SHUTDOWN, 0x00); /* shutdown mode */
    return ESP_OK;
}

static esp_err_t display_self_test(void)
{
    max7219_write_reg(MAX7219_REG_DISPLAYTEST, 0x01);
    vTaskDelay(pdMS_TO_TICKS(100));
    max7219_write_reg(MAX7219_REG_DISPLAYTEST, 0x00);
    return ESP_OK;
}

static esp_err_t display_command(const char *action, const char *params_json)
{
    if (strcmp(action, "set") != 0) return ESP_ERR_NOT_SUPPORTED;
    if (!params_json) return ESP_ERR_INVALID_ARG;

    char value_str[16] = {0};
    int  new_brightness = (int)s_cfg.brightness;

    /* ── Validate brightness (before any state change) ── */
    const char *bp = strstr(params_json, "\"brightness\":");
    if (bp) {
        bp += 13;
        while (*bp == ' ') bp++;
        char *endptr;
        long b = strtol(bp, &endptr, 10);
        if (endptr == bp) return ESP_ERR_INVALID_ARG; /* not a number */
        if (b < 0 || b > 15) return ESP_ERR_INVALID_ARG;
        new_brightness = (int)b;
    }

    /* ── Validate and parse value ── */
    const char *p = strstr(params_json, "\"value\":");
    if (!p) return ESP_ERR_INVALID_ARG;
    p += 8;
    while (*p == ' ') p++;

    if (*p == '"') {
        /* String value */
        p++;
        const char *end = strchr(p, '"');
        if (!end) return ESP_ERR_INVALID_ARG;
        size_t slen = (size_t)(end - p);
        if (slen > 5) slen = 5;
        memcpy(value_str, p, slen);
        value_str[slen] = '\0';
    } else if ((*p >= '0' && *p <= '9') || *p == '-') {
        /* Numeric value */
        char *endptr;
        long long n = strtoll(p, &endptr, 10);
        if (endptr == p) return ESP_ERR_INVALID_ARG;
        compact_number(n, value_str, sizeof(value_str));
    } else {
        return ESP_ERR_INVALID_ARG; /* null, bool, object, array */
    }

    /* ── Both validated: apply atomically ── */
    s_cfg.brightness = (uint8_t)new_brightness;
    max7219_write_reg(MAX7219_REG_INTENSITY, s_cfg.brightness);
    display_render(value_str);
    max7219_refresh();

    ESP_LOGI(TAG, "display.set: \"%s\" brightness=%d", value_str, new_brightness);
    return ESP_OK;
}

static esp_err_t display_mcp_set(const char *params_json, char *out, size_t out_len)
{
    esp_err_t err = display_command("set", params_json);
    if (err == ESP_OK) snprintf(out, out_len, "{\"status\":\"ok\"}");
    return err;
}

/* ── Registration ────────────────────────────────────────────────────────── */

void display_register(const char *instance, const void *config)
{
    static const char *schema_set =
        "{\"type\":\"object\","
        "\"properties\":{"
          "\"value\":{},"
          "\"brightness\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":15}"
        "},"
        "\"required\":[\"value\"]}";

    memset(&s_driver, 0, sizeof(s_driver));
    strlcpy(s_driver.instance,    instance,  sizeof(s_driver.instance));
    strlcpy(s_driver.driver_name, "display", sizeof(s_driver.driver_name));

    /* No WRITABLE capabilities — command.set delegated via command hook */
    s_driver.capability_count = 0;

    s_driver.init      = display_init;
    s_driver.deinit    = display_deinit;
    s_driver.command   = display_command;
    s_driver.self_test = display_self_test;

    strlcpy(s_driver.mcp_tools[0].name, "display_set",
            sizeof(s_driver.mcp_tools[0].name));
    s_driver.mcp_tools[0].description      = "Set the LED matrix display value and optional brightness";
    s_driver.mcp_tools[0].input_schema_json = schema_set;
    s_driver.mcp_tools[0].handler           = display_mcp_set;
    s_driver.mcp_tool_count = 1;

    JETTYD_REGISTER_DRIVER(&s_driver);

    display_init(config);
}
