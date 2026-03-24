/**
 * @file bme280.c
 * @brief BME280 I2C temperature/humidity/pressure sensor driver.
 *
 * Implements Bosch integer compensation formulae for temperature,
 * pressure, and humidity. All calibration data is read once at init
 * and stored in static memory — no dynamic allocation after init.
 */

#include "bme280.h"
#include "jettyd_driver.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "drv_bme280";

/* BME280 register addresses */
#define BME280_REG_CHIP_ID    0xD0
#define BME280_REG_RESET      0xE0
#define BME280_REG_CTRL_HUM   0xF2
#define BME280_REG_STATUS     0xF3
#define BME280_REG_CTRL_MEAS  0xF4
#define BME280_REG_CONFIG     0xF5
#define BME280_REG_DATA_START 0xF7

#define BME280_CHIP_ID_VALUE  0x60
#define BME280_RESET_VALUE    0xB6

/* Calibration registers */
#define BME280_REG_CALIB_T1   0x88
#define BME280_REG_CALIB_H1   0xA1
#define BME280_REG_CALIB_H2   0xE1

/* Compensation parameters (Bosch datasheet) */
typedef struct {
    uint16_t dig_T1;
    int16_t  dig_T2;
    int16_t  dig_T3;
    uint16_t dig_P1;
    int16_t  dig_P2;
    int16_t  dig_P3;
    int16_t  dig_P4;
    int16_t  dig_P5;
    int16_t  dig_P6;
    int16_t  dig_P7;
    int16_t  dig_P8;
    int16_t  dig_P9;
    uint8_t  dig_H1;
    int16_t  dig_H2;
    uint8_t  dig_H3;
    int16_t  dig_H4;
    int16_t  dig_H5;
    int8_t   dig_H6;
} bme280_calib_t;

static bme280_config_t s_cfg;
static jettyd_driver_t s_driver;
static bme280_calib_t  s_calib;
static volatile int32_t s_t_fine;

static esp_err_t i2c_read_reg(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_write_read_device(
        s_cfg.i2c_port, s_cfg.i2c_addr,
        &reg, 1, buf, len, pdMS_TO_TICKS(50));
}

static esp_err_t i2c_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t data[2] = {reg, val};
    return i2c_master_write_to_device(
        s_cfg.i2c_port, s_cfg.i2c_addr,
        data, sizeof(data), pdMS_TO_TICKS(50));
}

static esp_err_t read_calibration(void)
{
    uint8_t buf[26];
    esp_err_t err;

    /* Temperature and pressure calibration: 0x88..0xA1 (26 bytes) */
    err = i2c_read_reg(BME280_REG_CALIB_T1, buf, 26);
    if (err != ESP_OK) return err;

    s_calib.dig_T1 = (uint16_t)(buf[1] << 8 | buf[0]);
    s_calib.dig_T2 = (int16_t)(buf[3] << 8 | buf[2]);
    s_calib.dig_T3 = (int16_t)(buf[5] << 8 | buf[4]);
    s_calib.dig_P1 = (uint16_t)(buf[7] << 8 | buf[6]);
    s_calib.dig_P2 = (int16_t)(buf[9] << 8 | buf[8]);
    s_calib.dig_P3 = (int16_t)(buf[11] << 8 | buf[10]);
    s_calib.dig_P4 = (int16_t)(buf[13] << 8 | buf[12]);
    s_calib.dig_P5 = (int16_t)(buf[15] << 8 | buf[14]);
    s_calib.dig_P6 = (int16_t)(buf[17] << 8 | buf[16]);
    s_calib.dig_P7 = (int16_t)(buf[19] << 8 | buf[18]);
    s_calib.dig_P8 = (int16_t)(buf[21] << 8 | buf[20]);
    s_calib.dig_P9 = (int16_t)(buf[23] << 8 | buf[22]);

    /* Humidity calibration byte at 0xA1 */
    err = i2c_read_reg(BME280_REG_CALIB_H1, &s_calib.dig_H1, 1);
    if (err != ESP_OK) return err;

    /* Humidity calibration: 0xE1..0xE7 (7 bytes) */
    uint8_t hbuf[7];
    err = i2c_read_reg(BME280_REG_CALIB_H2, hbuf, 7);
    if (err != ESP_OK) return err;

    s_calib.dig_H2 = (int16_t)(hbuf[1] << 8 | hbuf[0]);
    s_calib.dig_H3 = hbuf[2];
    s_calib.dig_H4 = (int16_t)((hbuf[3] << 4) | (hbuf[4] & 0x0F));
    s_calib.dig_H5 = (int16_t)((hbuf[5] << 4) | (hbuf[4] >> 4));
    s_calib.dig_H6 = (int8_t)hbuf[6];

    return ESP_OK;
}

/* Bosch integer compensation: temperature (returns °C * 100) */
static int32_t compensate_temperature(int32_t adc_T)
{
    int32_t var1 = ((((adc_T >> 3) - ((int32_t)s_calib.dig_T1 << 1)))
                    * ((int32_t)s_calib.dig_T2)) >> 11;
    int32_t var2 = (((((adc_T >> 4) - ((int32_t)s_calib.dig_T1))
                      * ((adc_T >> 4) - ((int32_t)s_calib.dig_T1))) >> 12)
                    * ((int32_t)s_calib.dig_T3)) >> 14;
    s_t_fine = var1 + var2;
    return (s_t_fine * 5 + 128) >> 8;
}

/* Bosch integer compensation: pressure (returns Pa as Q24.8 fixed-point) */
static uint32_t compensate_pressure(int32_t adc_P)
{
    int64_t var1 = ((int64_t)s_t_fine) - 128000;
    int64_t var2 = var1 * var1 * (int64_t)s_calib.dig_P6;
    var2 = var2 + ((var1 * (int64_t)s_calib.dig_P5) << 17);
    var2 = var2 + (((int64_t)s_calib.dig_P4) << 35);
    var1 = ((var1 * var1 * (int64_t)s_calib.dig_P3) >> 8)
           + ((var1 * (int64_t)s_calib.dig_P2) << 12);
    var1 = (((((int64_t)1) << 47) + var1)) * ((int64_t)s_calib.dig_P1) >> 33;

    if (var1 == 0) return 0;

    int64_t p = 1048576 - adc_P;
    p = (((p << 31) - var2) * 3125) / var1;
    var1 = (((int64_t)s_calib.dig_P9) * (p >> 13) * (p >> 13)) >> 25;
    var2 = (((int64_t)s_calib.dig_P8) * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (((int64_t)s_calib.dig_P7) << 4);
    return (uint32_t)p;
}

/* Bosch integer compensation: humidity (returns %RH as Q22.10 fixed-point) */
static uint32_t compensate_humidity(int32_t adc_H)
{
    int32_t v_x1_u32r = s_t_fine - (int32_t)76800;
    v_x1_u32r = (((((adc_H << 14) - (((int32_t)s_calib.dig_H4) << 20)
                    - (((int32_t)s_calib.dig_H5) * v_x1_u32r)) + (int32_t)16384) >> 15)
                 * (((((((v_x1_u32r * ((int32_t)s_calib.dig_H6)) >> 10)
                        * (((v_x1_u32r * ((int32_t)s_calib.dig_H3)) >> 11)
                           + (int32_t)32768)) >> 10) + (int32_t)2097152)
                     * ((int32_t)s_calib.dig_H2) + 8192) >> 14));
    v_x1_u32r = v_x1_u32r - (((((v_x1_u32r >> 15) * (v_x1_u32r >> 15)) >> 7)
                               * ((int32_t)s_calib.dig_H1)) >> 4);
    v_x1_u32r = (v_x1_u32r < 0) ? 0 : v_x1_u32r;
    v_x1_u32r = (v_x1_u32r > 419430400) ? 419430400 : v_x1_u32r;
    return (uint32_t)(v_x1_u32r >> 12);
}

static esp_err_t bme280_init(const void *config)
{
    const bme280_config_t *c = (const bme280_config_t *)config;
    s_cfg = *c;

    if (s_cfg.i2c_addr == 0) s_cfg.i2c_addr = 0x76;
    if (s_cfg.oversampling == 0) s_cfg.oversampling = 1;

    /* Verify chip ID */
    uint8_t chip_id = 0;
    esp_err_t err = i2c_read_reg(BME280_REG_CHIP_ID, &chip_id, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C read chip ID failed: %s", esp_err_to_name(err));
        return err;
    }
    if (chip_id != BME280_CHIP_ID_VALUE) {
        ESP_LOGE(TAG, "Unexpected chip ID: 0x%02x (expected 0x%02x)",
                 chip_id, BME280_CHIP_ID_VALUE);
        return ESP_ERR_NOT_FOUND;
    }

    /* Soft reset */
    err = i2c_write_reg(BME280_REG_RESET, BME280_RESET_VALUE);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Read calibration data */
    err = read_calibration();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read calibration: %s", esp_err_to_name(err));
        return err;
    }

    /* Configure: humidity oversampling (must be written before ctrl_meas) */
    uint8_t osrs_h = s_cfg.oversampling & 0x07;
    err = i2c_write_reg(BME280_REG_CTRL_HUM, osrs_h);
    if (err != ESP_OK) return err;

    /* Config: standby=1000ms, filter=off */
    err = i2c_write_reg(BME280_REG_CONFIG, 0x05 << 5);
    if (err != ESP_OK) return err;

    /* ctrl_meas: temp osrs | press osrs | normal mode */
    uint8_t osrs_t = s_cfg.oversampling & 0x07;
    uint8_t osrs_p = s_cfg.oversampling & 0x07;
    uint8_t ctrl_meas = (osrs_t << 5) | (osrs_p << 2) | 0x03;
    err = i2c_write_reg(BME280_REG_CTRL_MEAS, ctrl_meas);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "BME280 init: addr=0x%02x, osrs=%d", s_cfg.i2c_addr, s_cfg.oversampling);
    return ESP_OK;
}

static esp_err_t bme280_deinit(void)
{
    /* Put sensor to sleep mode */
    i2c_write_reg(BME280_REG_CTRL_MEAS, 0x00);
    return ESP_OK;
}

static jettyd_value_t bme280_read(const char *capability)
{
    jettyd_value_t val = {.type = JETTYD_VAL_FLOAT, .valid = false};

    /* Read all 8 data bytes: press[3] + temp[3] + hum[2] */
    uint8_t data[8];
    esp_err_t err = i2c_read_reg(BME280_REG_DATA_START, data, 8);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2C data read failed: %s", esp_err_to_name(err));
        return val;
    }

    int32_t adc_P = (int32_t)((data[0] << 12) | (data[1] << 4) | (data[2] >> 4));
    int32_t adc_T = (int32_t)((data[3] << 12) | (data[4] << 4) | (data[5] >> 4));
    int32_t adc_H = (int32_t)((data[6] << 8) | data[7]);

    /* Temperature must be computed first (sets s_t_fine) */
    int32_t temp_raw = compensate_temperature(adc_T);

    if (strcmp(capability, "temperature") == 0) {
        val.float_val = (float)temp_raw / 100.0f;
        val.valid = true;
    } else if (strcmp(capability, "humidity") == 0) {
        uint32_t hum_raw = compensate_humidity(adc_H);
        val.float_val = (float)hum_raw / 1024.0f;
        if (val.float_val < 0.0f) val.float_val = 0.0f;
        if (val.float_val > 100.0f) val.float_val = 100.0f;
        val.valid = true;
    } else if (strcmp(capability, "pressure") == 0) {
        uint32_t press_raw = compensate_pressure(adc_P);
        /* Q24.8 Pa → hPa */
        val.float_val = (float)press_raw / 256.0f / 100.0f;
        val.valid = true;
    }

    return val;
}

static esp_err_t bme280_self_test(void)
{
    uint8_t chip_id = 0;
    esp_err_t err = i2c_read_reg(BME280_REG_CHIP_ID, &chip_id, 1);
    if (err != ESP_OK) return err;
    return (chip_id == BME280_CHIP_ID_VALUE) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

void bme280_register(const char *instance, const void *config)
{
    bme280_init(config);

    memset(&s_driver, 0, sizeof(s_driver));
    strncpy(s_driver.instance, instance, JETTYD_MAX_INSTANCE_NAME - 1);
    strncpy(s_driver.driver_name, "bme280", sizeof(s_driver.driver_name) - 1);

    s_driver.capability_count = 3;

    strncpy(s_driver.capabilities[0].name, "temperature", sizeof(s_driver.capabilities[0].name) - 1);
    s_driver.capabilities[0].type = JETTYD_CAP_READABLE;
    s_driver.capabilities[0].value_type = JETTYD_VAL_FLOAT;
    s_driver.capabilities[0].min_value = -40;
    s_driver.capabilities[0].max_value = 85;
    strncpy(s_driver.capabilities[0].unit, "C", sizeof(s_driver.capabilities[0].unit) - 1);

    strncpy(s_driver.capabilities[1].name, "humidity", sizeof(s_driver.capabilities[1].name) - 1);
    s_driver.capabilities[1].type = JETTYD_CAP_READABLE;
    s_driver.capabilities[1].value_type = JETTYD_VAL_FLOAT;
    s_driver.capabilities[1].min_value = 0;
    s_driver.capabilities[1].max_value = 100;
    strncpy(s_driver.capabilities[1].unit, "%", sizeof(s_driver.capabilities[1].unit) - 1);

    strncpy(s_driver.capabilities[2].name, "pressure", sizeof(s_driver.capabilities[2].name) - 1);
    s_driver.capabilities[2].type = JETTYD_CAP_READABLE;
    s_driver.capabilities[2].value_type = JETTYD_VAL_FLOAT;
    s_driver.capabilities[2].min_value = 300;
    s_driver.capabilities[2].max_value = 1100;
    strncpy(s_driver.capabilities[2].unit, "hPa", sizeof(s_driver.capabilities[2].unit) - 1);

    s_driver.init = bme280_init;
    s_driver.deinit = bme280_deinit;
    s_driver.read = bme280_read;
    s_driver.self_test = bme280_self_test;

    JETTYD_REGISTER_DRIVER(&s_driver);
}
