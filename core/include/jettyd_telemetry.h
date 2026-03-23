/**
 * @file jettyd_telemetry.h
 * @brief Telemetry publishing for the Jettyd firmware SDK.
 *
 * Builds and publishes telemetry messages in the format:
 * { "ts": <unix_ts>, "readings": { "instance.capability": value, ... } }
 */

#ifndef JETTYD_TELEMETRY_H
#define JETTYD_TELEMETRY_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "jettyd_driver.h"

/** Maximum metrics in a single telemetry message */
#define JETTYD_TELEMETRY_MAX_METRICS    32

/** Maximum telemetry JSON payload size */
#define JETTYD_TELEMETRY_MAX_PAYLOAD    1024

/**
 * @brief Initialize the telemetry subsystem.
 */
esp_err_t jettyd_telemetry_init(void);

/**
 * @brief Publish a telemetry message with specified metrics.
 *
 * Reads the current value of each metric from the driver registry
 * and publishes to the telemetry topic.
 *
 * @param metrics      Array of dotted metric names (e.g., "soil.moisture").
 * @param metric_count Number of metrics. If 0, publishes all available.
 * @return ESP_OK on success.
 */
esp_err_t jettyd_telemetry_publish(const char **metrics, uint8_t metric_count);

/**
 * @brief Publish a telemetry message with all available metrics.
 *
 * Reads every registered driver and publishes all readable capabilities
 * plus system metrics (battery, rssi, uptime, heap_free).
 */
esp_err_t jettyd_telemetry_publish_all(void);

/**
 * @brief Publish an alert message.
 *
 * @param message   Alert message (template-substituted).
 * @param severity  "info", "warning", or "critical".
 */
esp_err_t jettyd_telemetry_publish_alert(const char *message, const char *severity);

/**
 * @brief Publish device status.
 *
 * @param status  "online", "offline", "error", or "sleep".
 */
esp_err_t jettyd_telemetry_publish_status(const char *status);

/**
 * @brief Get system metrics (battery, rssi, uptime, heap_free).
 *
 * @param battery   Output: battery voltage (0 if no battery).
 * @param rssi      Output: WiFi RSSI in dBm.
 * @param uptime    Output: seconds since boot.
 * @param heap_free Output: free heap bytes.
 */
void jettyd_telemetry_get_system_metrics(float *battery, int8_t *rssi,
                                          uint32_t *uptime, uint32_t *heap_free);

#endif /* JETTYD_TELEMETRY_H */
