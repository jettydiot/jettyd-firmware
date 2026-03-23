/**
 * @file dht22.h
 * @brief DHT22 temperature and humidity sensor driver.
 */

#ifndef JETTYD_DRIVER_DHT22_H
#define JETTYD_DRIVER_DHT22_H

#include "jettyd_driver.h"

typedef struct {
    uint8_t pin;    /**< GPIO pin connected to DHT22 data line */
} dht22_config_t;

void dht22_register(const char *instance, const void *config);

#endif /* JETTYD_DRIVER_DHT22_H */
