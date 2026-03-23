/**
 * @file ds18b20.c
 * @brief DS18B20 temperature sensor — Dallas 1-Wire protocol, bit-banged.
 *
 * The DS18B20 is commonly used in waterproof probes for soil/water
 * temperature measurement. It provides 9-12 bit resolution.
 */

#include "ds18b20.h"
#include "jettyd_driver.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "rom/ets_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "drv_ds18b20";

static ds18b20_config_t s_cfg;
static jettyd_driver_t s_driver;
static float s_temperature = 0.0f;
static bool s_valid = false;

/* ───────────────────────────── 1-Wire Primitives ──────────────────────────── */

static void ow_set_output(gpio_num_t pin)
{
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
}

static void ow_set_input(gpio_num_t pin)
{
    gpio_set_direction(pin, GPIO_MODE_INPUT);
}

/**
 * @brief Send a 1-Wire reset pulse and detect device presence.
 * @return true if a device responded with a presence pulse.
 */
static bool ow_reset(gpio_num_t pin)
{
    ow_set_output(pin);
    gpio_set_level(pin, 0);
    ets_delay_us(480);
    ow_set_input(pin);
    ets_delay_us(70);

    bool present = (gpio_get_level(pin) == 0);
    ets_delay_us(410);
    return present;
}

static void ow_write_bit(gpio_num_t pin, int bit)
{
    ow_set_output(pin);
    if (bit) {
        gpio_set_level(pin, 0);
        ets_delay_us(6);
        ow_set_input(pin);
        ets_delay_us(64);
    } else {
        gpio_set_level(pin, 0);
        ets_delay_us(60);
        ow_set_input(pin);
        ets_delay_us(10);
    }
}

static int ow_read_bit(gpio_num_t pin)
{
    ow_set_output(pin);
    gpio_set_level(pin, 0);
    ets_delay_us(6);
    ow_set_input(pin);
    ets_delay_us(9);
    int val = gpio_get_level(pin);
    ets_delay_us(55);
    return val;
}

static void ow_write_byte(gpio_num_t pin, uint8_t byte)
{
    for (int i = 0; i < 8; i++) {
        ow_write_bit(pin, byte & 0x01);
        byte >>= 1;
    }
}

static uint8_t ow_read_byte(gpio_num_t pin)
{
    uint8_t val = 0;
    for (int i = 0; i < 8; i++) {
        val >>= 1;
        if (ow_read_bit(pin)) {
            val |= 0x80;
        }
    }
    return val;
}

/* ───────────────────────────── DS18B20 Commands ───────────────────────────── */

#define DS18B20_CMD_CONVERT     0x44
#define DS18B20_CMD_READ_SCRATCH 0xBE
#define DS18B20_CMD_SKIP_ROM    0xCC

/**
 * @brief Start temperature conversion and read the result.
 *
 * We use Skip ROM (assumes single device on bus) for simplicity.
 */
static esp_err_t ds18b20_read_temp(float *temp)
{
    gpio_num_t pin = (gpio_num_t)s_cfg.pin;

    /* Reset + Skip ROM + Convert */
    if (!ow_reset(pin)) {
        return ESP_ERR_NOT_FOUND;
    }
    ow_write_byte(pin, DS18B20_CMD_SKIP_ROM);
    ow_write_byte(pin, DS18B20_CMD_CONVERT);

    /* Wait for conversion (750ms at 12-bit resolution) */
    int wait_ms = 750;
    switch (s_cfg.resolution) {
    case 9:  wait_ms = 94;  break;
    case 10: wait_ms = 188; break;
    case 11: wait_ms = 375; break;
    case 12: wait_ms = 750; break;
    }
    vTaskDelay(pdMS_TO_TICKS(wait_ms));

    /* Reset + Skip ROM + Read Scratchpad */
    if (!ow_reset(pin)) {
        return ESP_ERR_NOT_FOUND;
    }
    ow_write_byte(pin, DS18B20_CMD_SKIP_ROM);
    ow_write_byte(pin, DS18B20_CMD_READ_SCRATCH);

    /* Read 9 bytes of scratchpad */
    uint8_t data[9];
    for (int i = 0; i < 9; i++) {
        data[i] = ow_read_byte(pin);
    }

    /* CRC check (byte 8 is CRC of bytes 0-7) */
    uint8_t crc = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t byte = data[i];
        for (int j = 0; j < 8; j++) {
            uint8_t mix = (crc ^ byte) & 0x01;
            crc >>= 1;
            if (mix) crc ^= 0x8C;
            byte >>= 1;
        }
    }
    if (crc != data[8]) {
        ESP_LOGW(TAG, "CRC mismatch");
        return ESP_ERR_INVALID_CRC;
    }

    /* Parse temperature from bytes 0-1 */
    int16_t raw = (data[1] << 8) | data[0];
    *temp = raw / 16.0f;

    return ESP_OK;
}

/* ───────────────────────────── Driver Interface ───────────────────────────── */

static esp_err_t ds18b20_init(const void *config)
{
    const ds18b20_config_t *c = (const ds18b20_config_t *)config;
    s_cfg = *c;
    if (s_cfg.resolution < 9 || s_cfg.resolution > 12) {
        s_cfg.resolution = 12;
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << s_cfg.pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    ESP_LOGI(TAG, "DS18B20 init: pin=%d, resolution=%d bit", s_cfg.pin, s_cfg.resolution);
    return ESP_OK;
}

static jettyd_value_t ds18b20_read(const char *capability)
{
    jettyd_value_t val = {.type = JETTYD_VAL_FLOAT, .valid = false};

    float temp;
    esp_err_t err = ds18b20_read_temp(&temp);
    if (err == ESP_OK) {
        s_temperature = temp;
        s_valid = true;
    }

    if (s_valid && strcmp(capability, "temperature") == 0) {
        val.float_val = s_temperature;
        val.valid = true;
    }

    return val;
}

static esp_err_t ds18b20_self_test(void)
{
    gpio_num_t pin = (gpio_num_t)s_cfg.pin;
    return ow_reset(pin) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

void ds18b20_register(const char *instance, const void *config)
{
    ds18b20_init(config);

    memset(&s_driver, 0, sizeof(s_driver));
    strncpy(s_driver.instance, instance, JETTYD_MAX_INSTANCE_NAME - 1);
    strncpy(s_driver.driver_name, "ds18b20", sizeof(s_driver.driver_name) - 1);

    s_driver.capability_count = 1;
    strncpy(s_driver.capabilities[0].name, "temperature",
            sizeof(s_driver.capabilities[0].name) - 1);
    s_driver.capabilities[0].type = JETTYD_CAP_READABLE;
    s_driver.capabilities[0].value_type = JETTYD_VAL_FLOAT;
    s_driver.capabilities[0].min_value = -55.0f;
    s_driver.capabilities[0].max_value = 125.0f;
    strncpy(s_driver.capabilities[0].unit, "\xC2\xB0""C",
            sizeof(s_driver.capabilities[0].unit) - 1);

    s_driver.init = ds18b20_init;
    s_driver.read = ds18b20_read;
    s_driver.self_test = ds18b20_self_test;

    JETTYD_REGISTER_DRIVER(&s_driver);
}
