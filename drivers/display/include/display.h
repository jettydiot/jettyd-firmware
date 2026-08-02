/**
 * @file display.h
 * @brief MAX7219 LED matrix display driver for the Jettyd firmware SDK.
 *
 * Bit-bang SPI, FC16-style daisy-chain layout, 32x8 usable pixels (4 modules).
 * Accepts display.set {value, brightness} via the command hook.
 */

#ifndef JETTYD_DRIVER_DISPLAY_H
#define JETTYD_DRIVER_DISPLAY_H

#include "jettyd_driver.h"
#include <stdint.h>

/**
 * @brief Display driver configuration.
 */
typedef struct {
    uint8_t pin_din;       /**< GPIO pin for SPI DIN (data in) */
    uint8_t pin_clk;       /**< GPIO pin for SPI CLK */
    uint8_t pin_cs;        /**< GPIO pin for SPI CS (LOAD) */
    uint8_t num_modules;   /**< Number of daisy-chained MAX7219 modules (max 4) */
    uint8_t brightness;    /**< Initial intensity, 0–15 */
} display_config_t;

/**
 * @brief Register a MAX7219 display driver instance.
 *
 * @param instance  Instance name (e.g., "panel").
 * @param config    Pointer to display_config_t.
 */
void display_register(const char *instance, const void *config);

#endif /* JETTYD_DRIVER_DISPLAY_H */
