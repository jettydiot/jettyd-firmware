/**
 * @file button.h
 * @brief Button/switch driver — GPIO input with debounce and press event publishing.
 */
#ifndef JETTYD_DRIVER_BUTTON_H
#define JETTYD_DRIVER_BUTTON_H

#include "jettyd_driver.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t  pin;              /**< GPIO pin connected to button */
    bool     active_low;       /**< true = button connects pin to GND (use internal pull-up) */
    uint32_t debounce_ms;      /**< Debounce time in ms (default: 50) */
    uint32_t long_press_ms;    /**< Hold duration for long press event in ms (default: 500, 0 = disabled) */
    uint32_t double_press_ms;  /**< Max gap between presses for double-press in ms (default: 300, 0 = disabled) */
} button_config_t;

void button_register(const char *instance, const void *config);

#endif /* JETTYD_DRIVER_BUTTON_H */
