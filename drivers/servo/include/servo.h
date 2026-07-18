/**
 * @file servo.h
 * @brief Hobby servo driver for the Jettyd firmware SDK.
 */

#ifndef JETTYD_DRIVER_SERVO_H
#define JETTYD_DRIVER_SERVO_H

#include "jettyd_driver.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Servo driver configuration.
 */
typedef struct {
    uint8_t  pin;             /**< GPIO pin driving the servo signal wire (required) */
    uint8_t  ledc_channel;    /**< LEDC channel (0-7), default 0 */
    uint32_t min_pulse_us;    /**< Pulse width at angle 0, default 500 */
    uint32_t max_pulse_us;    /**< Pulse width at max_angle, default 2500 */
    float    max_angle;       /**< Maximum commandable angle in degrees, default 180 */
    float    home_angle;      /**< Angle to return to after a rotate command, default 0 */
    bool     idle_detach;     /**< Detach LEDC signal ~500ms after motion completes, default true */
} servo_config_t;

/**
 * @brief Register a servo driver instance.
 *
 * @param instance  Instance name (e.g., "gate", "arm").
 * @param config    Pointer to servo_config_t.
 */
void servo_register(const char *instance, const void *config);

#endif /* JETTYD_DRIVER_SERVO_H */
