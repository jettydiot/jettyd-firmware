/**
 * @file led.h
 * @brief LED driver — GPIO output with on/off/blink control.
 */
#ifndef JETTYD_DRIVER_LED_H
#define JETTYD_DRIVER_LED_H

#include "jettyd_driver.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t pin;          /**< GPIO pin connected to LED (via resistor) */
    bool    active_high;  /**< true = GPIO high turns LED on (default) */
} led_config_t;

void led_register(const char *instance, const void *config);

#endif /* JETTYD_DRIVER_LED_H */
