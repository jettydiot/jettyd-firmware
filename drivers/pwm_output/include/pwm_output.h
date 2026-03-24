/**
 * @file pwm_output.h
 * @brief LEDC PWM output driver for the Jettyd firmware SDK.
 */

#ifndef JETTYD_DRIVER_PWM_OUTPUT_H
#define JETTYD_DRIVER_PWM_OUTPUT_H

#include "jettyd_driver.h"

typedef struct {
    uint8_t gpio_pin;           /**< GPIO pin for PWM output */
    uint32_t frequency_hz;      /**< PWM frequency in Hz (default 1000) */
    uint8_t ledc_channel;       /**< LEDC channel number */
    uint8_t ledc_timer;         /**< LEDC timer number */
} pwm_output_config_t;

void pwm_output_register(const char *instance, const void *config);

#endif /* JETTYD_DRIVER_PWM_OUTPUT_H */
