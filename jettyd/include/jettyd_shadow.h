/**
 * @file jettyd_shadow.h
 * @brief Device shadow interface for the Jettyd firmware SDK.
 *
 * Maintains a local shadow — a JSON document representing the device's
 * current state. Published to shadow/report on connect and on every
 * state change.
 */

#ifndef JETTYD_SHADOW_H
#define JETTYD_SHADOW_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "jettyd_driver.h"

/** Maximum shadow JSON size */
#define JETTYD_SHADOW_MAX_SIZE  1024

/**
 * @brief Initialize the shadow subsystem.
 *
 * Loads the last shadow from NVS (if available).
 */
esp_err_t jettyd_shadow_init(void);

/**
 * @brief Update a single reading in the shadow.
 *
 * @param dotted_name  e.g., "soil.moisture", "system.battery"
 * @param value        The new value.
 */
esp_err_t jettyd_shadow_update(const char *dotted_name, jettyd_value_t value);

/**
 * @brief Update a system metric in the shadow.
 *
 * @param key    e.g., "firmware_version", "uptime"
 * @param value  The value.
 */
esp_err_t jettyd_shadow_update_system(const char *key, jettyd_value_t value);

/**
 * @brief Set the desired state (from platform shadow/desired topic).
 *
 * @param json  Raw JSON string of the desired state.
 * @param len   JSON length.
 */
esp_err_t jettyd_shadow_set_desired(const char *json, int len);

/**
 * @brief Publish the full shadow to MQTT shadow/report topic.
 */
esp_err_t jettyd_shadow_publish(void);

/**
 * @brief Persist the current shadow to NVS.
 */
esp_err_t jettyd_shadow_persist(void);

/**
 * @brief Reconcile desired vs reported state.
 *
 * Checks if there are differences between desired and reported state
 * and executes the necessary driver actions to reach desired state.
 */
esp_err_t jettyd_shadow_reconcile(void);

/**
 * @brief MQTT callback for the shadow/desired topic.
 */
void jettyd_shadow_desired_handler(const char *topic, const char *data, int data_len);

/**
 * @brief Get the shadow JSON string for reporting.
 *
 * @param buf      Output buffer.
 * @param buf_len  Buffer size.
 * @return Number of bytes written, or -1 on error.
 */
int jettyd_shadow_serialize(char *buf, size_t buf_len);

#endif /* JETTYD_SHADOW_H */
