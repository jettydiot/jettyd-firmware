/**
 * @file bme280.h
 * @brief BME280 I2C temperature/humidity/pressure sensor driver.
 */

#ifndef JETTYD_DRIVER_BME280_H
#define JETTYD_DRIVER_BME280_H

#include "jettyd_driver.h"

typedef struct {
    uint8_t i2c_port;           /**< I2C port number */
    uint8_t sda_pin;            /**< SDA GPIO pin */
    uint8_t scl_pin;            /**< SCL GPIO pin */
    uint8_t i2c_addr;           /**< I2C address (default 0x76) */
    uint8_t oversampling;       /**< Oversampling setting (1-5, maps to x1-x16) */
} bme280_config_t;

void bme280_register(const char *instance, const void *config);

#endif /* JETTYD_DRIVER_BME280_H */
