/**
 * @file ds18b20.h
 * @brief DS18B20 waterproof temperature sensor driver (Dallas 1-Wire).
 */

#ifndef JETTYD_DRIVER_DS18B20_H
#define JETTYD_DRIVER_DS18B20_H

#include "jettyd_driver.h"

typedef struct {
    uint8_t pin;            /**< GPIO pin for 1-Wire data line */
    uint8_t resolution;     /**< 9, 10, 11, or 12 bits (default 12) */
} ds18b20_config_t;

void ds18b20_register(const char *instance, const void *config);

#endif /* JETTYD_DRIVER_DS18B20_H */
