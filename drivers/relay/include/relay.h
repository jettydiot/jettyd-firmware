/**
 * @file relay.h
 * @brief Relay driver for the Jettyd firmware SDK.
 */

#ifndef JETTYD_DRIVER_RELAY_H
#define JETTYD_DRIVER_RELAY_H

#include "jettyd_driver.h"

/**
 * @brief Relay driver configuration.
 */
typedef struct {
    uint8_t pin;                /**< GPIO pin number */
    bool active_high;           /**< true = HIGH activates relay */
    bool default_state_on;      /**< Initial state */
    uint32_t max_on_duration;   /**< Safety auto-off in seconds (0 = no limit) */
} relay_config_t;

/**
 * @brief Register a relay driver instance.
 *
 * @param instance  Instance name (e.g., "valve").
 * @param config    Pointer to relay_config_t.
 */
void relay_register(const char *instance, const void *config);

#endif /* JETTYD_DRIVER_RELAY_H */
