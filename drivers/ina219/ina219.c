/**
 * @file ina219.c
 * @brief INA219 I2C voltage/current/power monitor driver.
 *
 * Reads bus voltage, shunt voltage, and computes current and power
 * using calibration register. No dynamic allocation after init.
 */

#include "ina219.h"
#include "jettyd_driver.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "drv_ina219";

/* INA219 register addresses */
#define INA219_REG_CONFIG       0x00
#define INA219_REG_SHUNT_V      0x01
#define INA219_REG_BUS_V        0x02
#define INA219_REG_POWER        0x03
#define INA219_REG_CURRENT      0x04
#define INA219_REG_CALIBRATION  0x05

/* Default config: 32V range, ±320mV shunt, 12-bit, continuous */
#define INA219_DEFAULT_CONFIG   0x399F

static ina219_config_t s_cfg;
static jettyd_driver_t s_driver;
static volatile float s_current_lsb;

static esp_err_t i2c_read_reg16(uint8_t reg, uint16_t *val)
{
    uint8_t buf[2];
    esp_err_t err = i2c_master_write_read_device(
        s_cfg.i2c_port, s_cfg.i2c_addr,
        &reg, 1, buf, 2, pdMS_TO_TICKS(50));
    if (err != ESP_OK) return err;
    *val = (uint16_t)((buf[0] << 8) | buf[1]);
    return ESP_OK;
}

static esp_err_t i2c_write_reg16(uint8_t reg, uint16_t val)
{
    uint8_t data[3] = {reg, (uint8_t)(val >> 8), (uint8_t)(val & 0xFF)};
    return i2c_master_write_to_device(
        s_cfg.i2c_port, s_cfg.i2c_addr,
        data, sizeof(data), pdMS_TO_TICKS(50));
}

static esp_err_t ina219_init(const void *config)
{
    const ina219_config_t *c = (const ina219_config_t *)config;
    s_cfg = *c;

    if (s_cfg.i2c_addr == 0) s_cfg.i2c_addr = 0x40;
    if (s_cfg.shunt_resistance_mohm == 0) s_cfg.shunt_resistance_mohm = 100;

    /* Write default configuration */
    esp_err_t err = i2c_write_reg16(INA219_REG_CONFIG, INA219_DEFAULT_CONFIG);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Config write failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Calculate and write calibration register
     * Max expected current = 3.2A (for ±320mV range with 100mΩ shunt)
     * current_lsb = max_current / 2^15 ≈ 0.0001A (100µA)
     * Cal = trunc(0.04096 / (current_lsb * shunt_resistance))
     */
    float shunt_r = (float)s_cfg.shunt_resistance_mohm / 1000.0f;
    float max_current = 0.32f / shunt_r;
    s_current_lsb = max_current / 32768.0f;
    uint16_t cal = (uint16_t)(0.04096f / (s_current_lsb * shunt_r));

    err = i2c_write_reg16(INA219_REG_CALIBRATION, cal);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Calibration write failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "INA219 init: addr=0x%02x, shunt=%u mΩ, cal=%u",
             s_cfg.i2c_addr, s_cfg.shunt_resistance_mohm, cal);
    return ESP_OK;
}

static esp_err_t ina219_deinit(void)
{
    return ESP_OK;
}

static jettyd_value_t ina219_read(const char *capability)
{
    jettyd_value_t val = {.type = JETTYD_VAL_FLOAT, .valid = false};

    if (strcmp(capability, "voltage") == 0) {
        uint16_t raw = 0;
        esp_err_t err = i2c_read_reg16(INA219_REG_BUS_V, &raw);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Bus voltage read failed: %s", esp_err_to_name(err));
            return val;
        }
        /* Bus voltage: bits [15:3] represent voltage, LSB = 4mV */
        val.float_val = (float)((raw >> 3) * 4) / 1000.0f;
        val.valid = true;
    } else if (strcmp(capability, "current") == 0) {
        uint16_t raw = 0;
        esp_err_t err = i2c_read_reg16(INA219_REG_CURRENT, &raw);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Current read failed: %s", esp_err_to_name(err));
            return val;
        }
        val.float_val = (float)(int16_t)raw * s_current_lsb;
        val.valid = true;
    } else if (strcmp(capability, "power") == 0) {
        uint16_t raw = 0;
        esp_err_t err = i2c_read_reg16(INA219_REG_POWER, &raw);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Power read failed: %s", esp_err_to_name(err));
            return val;
        }
        /* Power LSB = 20 * current_lsb */
        val.float_val = (float)raw * 20.0f * s_current_lsb;
        val.valid = true;
    }

    return val;
}

static esp_err_t ina219_self_test(void)
{
    uint16_t config_val = 0;
    esp_err_t err = i2c_read_reg16(INA219_REG_CONFIG, &config_val);
    if (err != ESP_OK) return err;
    /* Config register should not be zero if device is alive */
    return (config_val != 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

void ina219_register(const char *instance, const void *config)
{
    ina219_init(config);

    memset(&s_driver, 0, sizeof(s_driver));
    strncpy(s_driver.instance, instance, JETTYD_MAX_INSTANCE_NAME - 1);
    strncpy(s_driver.driver_name, "ina219", sizeof(s_driver.driver_name) - 1);

    s_driver.capability_count = 3;

    strncpy(s_driver.capabilities[0].name, "voltage", sizeof(s_driver.capabilities[0].name) - 1);
    s_driver.capabilities[0].type = JETTYD_CAP_READABLE;
    s_driver.capabilities[0].value_type = JETTYD_VAL_FLOAT;
    s_driver.capabilities[0].min_value = 0;
    s_driver.capabilities[0].max_value = 26;
    strncpy(s_driver.capabilities[0].unit, "V", sizeof(s_driver.capabilities[0].unit) - 1);

    strncpy(s_driver.capabilities[1].name, "current", sizeof(s_driver.capabilities[1].name) - 1);
    s_driver.capabilities[1].type = JETTYD_CAP_READABLE;
    s_driver.capabilities[1].value_type = JETTYD_VAL_FLOAT;
    s_driver.capabilities[1].min_value = -3.2f;
    s_driver.capabilities[1].max_value = 3.2f;
    strncpy(s_driver.capabilities[1].unit, "A", sizeof(s_driver.capabilities[1].unit) - 1);

    strncpy(s_driver.capabilities[2].name, "power", sizeof(s_driver.capabilities[2].name) - 1);
    s_driver.capabilities[2].type = JETTYD_CAP_READABLE;
    s_driver.capabilities[2].value_type = JETTYD_VAL_FLOAT;
    s_driver.capabilities[2].min_value = 0;
    s_driver.capabilities[2].max_value = 83.2f;
    strncpy(s_driver.capabilities[2].unit, "W", sizeof(s_driver.capabilities[2].unit) - 1);

    s_driver.init = ina219_init;
    s_driver.deinit = ina219_deinit;
    s_driver.read = ina219_read;
    s_driver.self_test = ina219_self_test;

    JETTYD_REGISTER_DRIVER(&s_driver);
}
