#pragma once
#include <stdint.h>

typedef struct {
    int sda_pin;
    int scl_pin;
    int i2c_port;       /**< I2C port number (0 or 1) */
    uint8_t i2c_addr;   /**< I2C address (default 0x65) */
} rgb_led_matrix_config_t;

void rgb_led_matrix_register(const char *instance, const void *config);
