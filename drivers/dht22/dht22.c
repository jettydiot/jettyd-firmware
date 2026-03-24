/**
 * @file dht22.c
 * @brief DHT22 (AM2302) temperature/humidity sensor — bit-banged OneWire protocol.
 *
 * The DHT22 uses a custom single-wire protocol (not Dallas 1-Wire).
 * Communication involves precise timing of GPIO transitions.
 */

#include "dht22.h"
#include "jettyd_driver.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <time.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"
#include <string.h>

static const char *TAG = "drv_dht22";

static dht22_config_t s_cfg;
static jettyd_driver_t s_driver;
static float s_temperature = 0.0f;
static float s_humidity = 0.0f;
static int64_t s_last_read_us = 0;

/* Minimum interval between reads (DHT22 spec: 2 seconds) */
#define DHT22_MIN_INTERVAL_US  2000000

/**
 * @brief Wait for a specific GPIO level with timeout.
 * @return Duration in microseconds, or -1 on timeout.
 */
static int wait_for_level(gpio_num_t pin, int level, int timeout_us)
{
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(pin) != level) {
        if ((esp_timer_get_time() - start) > timeout_us) {
            return -1;
        }
    }
    return (int)(esp_timer_get_time() - start);
}

/**
 * @brief Read 40 bits from the DHT22.
 *
 * Protocol:
 *   1. Pull data line LOW for >1ms (start signal)
 *   2. Release line, DHT22 responds with LOW 80us, HIGH 80us
 *   3. Each bit: LOW 50us, then HIGH 26-28us (0) or 70us (1)
 */
static esp_err_t dht22_read_raw(uint8_t data[5])
{
    gpio_num_t pin = (gpio_num_t)s_cfg.pin;

    /* Send start signal */
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, 0);
    ets_delay_us(1100);  /* Hold LOW for >1ms */
    gpio_set_level(pin, 1);
    ets_delay_us(30);

    /* Switch to input */
    gpio_set_direction(pin, GPIO_MODE_INPUT);

    /* Wait for DHT22 response: LOW 80us */
    if (wait_for_level(pin, 0, 100) < 0) return ESP_ERR_TIMEOUT;
    if (wait_for_level(pin, 1, 100) < 0) return ESP_ERR_TIMEOUT;
    if (wait_for_level(pin, 0, 100) < 0) return ESP_ERR_TIMEOUT;

    /* Read 40 bits (5 bytes) */
    memset(data, 0, 5);
    for (int i = 0; i < 40; i++) {
        /* Wait for HIGH (start of bit) */
        if (wait_for_level(pin, 1, 100) < 0) return ESP_ERR_TIMEOUT;

        /* Measure HIGH duration to determine bit value */
        int64_t start = esp_timer_get_time();
        if (wait_for_level(pin, 0, 100) < 0) return ESP_ERR_TIMEOUT;
        int duration = (int)(esp_timer_get_time() - start);

        /* >40us = 1, <40us = 0 */
        if (duration > 40) {
            data[i / 8] |= (1 << (7 - (i % 8)));
        }
    }

    /* Verify checksum */
    uint8_t checksum = data[0] + data[1] + data[2] + data[3];
    if (checksum != data[4]) {
        ESP_LOGW(TAG, "Checksum mismatch: %02x != %02x", checksum, data[4]);
        return ESP_ERR_INVALID_CRC;
    }

    return ESP_OK;
}

static esp_err_t dht22_update(void)
{
    int64_t now = esp_timer_get_time();
    if ((now - s_last_read_us) < DHT22_MIN_INTERVAL_US) {
        return ESP_OK; /* Use cached values */
    }

    uint8_t data[5];
    esp_err_t err = dht22_read_raw(data);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Read failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Parse humidity (0.1% resolution) */
    uint16_t raw_hum = (data[0] << 8) | data[1];
    s_humidity = raw_hum / 10.0f;

    /* Parse temperature (0.1°C resolution, bit 15 = negative) */
    uint16_t raw_temp = (data[2] << 8) | data[3];
    if (raw_temp & 0x8000) {
        raw_temp &= 0x7FFF;
        s_temperature = -(raw_temp / 10.0f);
    } else {
        s_temperature = raw_temp / 10.0f;
    }

    s_last_read_us = now;
    return ESP_OK;
}

static esp_err_t dht22_init(const void *config)
{
    const dht22_config_t *c = (const dht22_config_t *)config;
    s_cfg = *c;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << s_cfg.pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    ESP_LOGI(TAG, "DHT22 init: pin=%d", s_cfg.pin);
    return ESP_OK;
}

static jettyd_value_t dht22_read(const char *capability)
{
    jettyd_value_t val = {.type = JETTYD_VAL_FLOAT, .valid = false};

    esp_err_t err = dht22_update();
    if (err != ESP_OK && s_last_read_us == 0) {
        return val; /* Never had a successful read */
    }

    val.valid = true;
    if (strcmp(capability, "temperature") == 0) {
        val.float_val = s_temperature;
    } else if (strcmp(capability, "humidity") == 0) {
        val.float_val = s_humidity;
    } else {
        val.valid = false;
    }

    return val;
}

static esp_err_t dht22_self_test(void)
{
    uint8_t data[5];
    return dht22_read_raw(data);
}

void dht22_register(const char *instance, const void *config)
{
    dht22_init(config);

    memset(&s_driver, 0, sizeof(s_driver));
    strncpy(s_driver.instance, instance, JETTYD_MAX_INSTANCE_NAME - 1);
    strlcpy(s_driver.driver_name, "dht22", sizeof(s_driver.driver_name));

    s_driver.capability_count = 2;

    strlcpy(s_driver.capabilities[0].name, "temperature", sizeof(s_driver.capabilities[0].name));
    s_driver.capabilities[0].type = JETTYD_CAP_READABLE;
    s_driver.capabilities[0].value_type = JETTYD_VAL_FLOAT;
    s_driver.capabilities[0].min_value = -40.0f;
    s_driver.capabilities[0].max_value = 80.0f;
    strlcpy(s_driver.capabilities[0].unit, "\xC2\xB0""C", sizeof(s_driver.capabilities[0].unit));

    strlcpy(s_driver.capabilities[1].name, "humidity", sizeof(s_driver.capabilities[1].name));
    s_driver.capabilities[1].type = JETTYD_CAP_READABLE;
    s_driver.capabilities[1].value_type = JETTYD_VAL_FLOAT;
    s_driver.capabilities[1].min_value = 0.0f;
    s_driver.capabilities[1].max_value = 100.0f;
    strlcpy(s_driver.capabilities[1].unit, "%", sizeof(s_driver.capabilities[1].unit));

    s_driver.init = dht22_init;
    s_driver.read = dht22_read;
    s_driver.self_test = dht22_self_test;

    JETTYD_REGISTER_DRIVER(&s_driver);
}
