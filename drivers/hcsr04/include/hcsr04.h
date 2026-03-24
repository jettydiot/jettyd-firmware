/**
 * @file hcsr04.h
 * @brief HC-SR04 ultrasonic distance sensor driver.
 */

#ifndef JETTYD_DRIVER_HCSR04_H
#define JETTYD_DRIVER_HCSR04_H

#include "jettyd_driver.h"

typedef struct {
    uint8_t trigger_pin;        /**< GPIO pin for trigger pulse */
    uint8_t echo_pin;           /**< GPIO pin for echo input */
} hcsr04_config_t;

void hcsr04_register(const char *instance, const void *config);

#endif /* JETTYD_DRIVER_HCSR04_H */
