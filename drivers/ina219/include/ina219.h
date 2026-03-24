/**
 * @file ina219.h
 * @brief INA219 I2C voltage/current/power monitor driver.
 */

#ifndef JETTYD_DRIVER_INA219_H
#define JETTYD_DRIVER_INA219_H

#include "jettyd_driver.h"

typedef struct {
    uint8_t i2c_port;               /**< I2C port number */
    uint8_t sda_pin;                /**< SDA GPIO pin */
    uint8_t scl_pin;                /**< SCL GPIO pin */
    uint8_t i2c_addr;               /**< I2C address (default 0x40) */
    uint16_t shunt_resistance_mohm; /**< Shunt resistance in milliohms (default 100) */
} ina219_config_t;

void ina219_register(const char *instance, const void *config);

#endif /* JETTYD_DRIVER_INA219_H */
