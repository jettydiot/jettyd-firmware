/**
 * @file pwm_output.h
 * @brief PWM output driver for the Jettyd firmware SDK.
 */

#ifndef JETTYD_DRIVER_PWM_OUTPUT_H
#define JETTYD_DRIVER_PWM_OUTPUT_H

#include "jettyd_driver.h"

/**
 * @brief PWM output driver configuration.
 */
typedef struct {
    uint8_t pin;                /**< GPIO pin number */
    uint8_t ledc_channel;      /**< LEDC channel (0-7) */
    uint32_t freq_hz;          /**< PWM frequency in Hz */
    uint32_t max_on_duration;  /**< Auto-off after N seconds (0 = no limit, default 3600) */
} pwm_output_config_t;

/**
 * @brief Register a PWM output driver instance.
 *
 * @param instance  Instance name (e.g., "pump", "fan").
 * @param config    Pointer to pwm_output_config_t.
 */
void pwm_output_register(const char *instance, const void *config);

#endif /* JETTYD_DRIVER_PWM_OUTPUT_H */
