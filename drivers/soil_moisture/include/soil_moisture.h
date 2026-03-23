/**
 * @file soil_moisture.h
 * @brief Capacitive/resistive soil moisture sensor driver.
 */

#ifndef JETTYD_DRIVER_SOIL_MOISTURE_H
#define JETTYD_DRIVER_SOIL_MOISTURE_H

#include "jettyd_driver.h"

typedef struct {
    uint8_t pin;            /**< ADC-capable GPIO pin */
    char type[16];          /**< "capacitive" or "resistive" */
    uint16_t dry_value;     /**< Raw ADC reading when dry */
    uint16_t wet_value;     /**< Raw ADC reading when submerged */
    char unit[8];           /**< Unit string (default "%") */
} soil_moisture_config_t;

void soil_moisture_register(const char *instance, const void *config);

#endif /* JETTYD_DRIVER_SOIL_MOISTURE_H */
