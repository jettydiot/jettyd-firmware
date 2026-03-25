/**
 * @file jettyd.h
 * @brief Top-level API for the Jettyd firmware SDK.
 *
 * Device main.c calls jettyd_init() then jettyd_start(). Everything
 * else is handled by the core runtime.
 */

#ifndef JETTYD_H
#define JETTYD_H

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief Firmware version string, set at compile time from device.yaml.
 */
extern const char *JETTYD_FIRMWARE_VERSION;

/**
 * @brief Device type name, set at compile time from device.yaml.
 */
extern const char *JETTYD_DEVICE_TYPE;

/**
 * @brief Jettyd runtime configuration passed to jettyd_init().
 */
typedef struct {
    const char *device_type;            /**< e.g., "soil-moisture-v1" */
    const char *firmware_version;       /**< e.g., "1.0.0" */
    uint32_t heartbeat_interval_sec;    /**< Default heartbeat interval */
    const char **default_metrics;       /**< NULL-terminated list of default metrics */
    uint32_t mqtt_keepalive;            /**< MQTT keepalive in seconds */
    uint8_t mqtt_qos;                   /**< MQTT QoS level (0, 1, or 2) */
    bool mqtt_buffer_on_disconnect;     /**< Buffer messages when disconnected */
    uint16_t mqtt_max_buffer_size;      /**< Max buffered messages */
    bool deep_sleep;                    /**< Enable deep sleep between heartbeats */
    uint32_t sleep_duration_sec;        /**< Deep sleep duration (0 = use heartbeat_interval) */
    int wake_on_pin;                    /**< GPIO for wake from sleep (-1 = disabled) */
    bool has_battery;                   /**< Device has battery */
    int battery_adc_pin;                /**< ADC pin for battery voltage (-1 = none) */
    float battery_voltage_divider;      /**< Voltage divider ratio */
    int status_led_pin;                 /**< GPIO for status LED (-1 = none) */
} jettyd_config_t;

/**
 * @brief Initialize the Jettyd runtime.
 *
 * Initializes NVS, WiFi, MQTT, provisioning, shadow, and the driver
 * registry. Must be called before jettyd_start().
 *
 * @param config Runtime configuration from device.yaml.
 * @return ESP_OK on success, error code on failure.
 */
esp_err_t jettyd_init(const jettyd_config_t *config);

/**
 * @brief Register all compiled-in drivers (auto-generated or user-defined).
 * Implement this in your project to register drivers with the registry.
 */
/**
 * @brief Register application drivers.
 *
 * Define this function in your own main/ component to register your drivers.
 * The SDK provides a weak no-op default — you do NOT need to edit the SDK.
 *
 * Example in your main/driver_registry.c:
 * @code
 *   #include "led.h"
 *   void jettyd_register_drivers(void) {
 *       led_config_t cfg = { .pin = 8, .active_high = true };
 *       led_register("status", &cfg);
 *   }
 * @endcode
 */
void jettyd_register_drivers(void) __attribute__((weak));

/**
 * @brief Start the Jettyd runtime.
 *
 * Connects WiFi, performs provisioning if needed, connects MQTT,
 * registers drivers, loads JettyScript rules, starts the VM, and
 * enters the main loop. This function does not return.
 *
 * @return ESP_OK on success (never returns on success).
 */
esp_err_t jettyd_start(void);

/**
 * @brief Get the current runtime configuration.
 */
const jettyd_config_t *jettyd_get_config(void);

#endif /* JETTYD_H */
