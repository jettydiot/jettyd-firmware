/**
 * @file jettyd_wifi.h
 * @brief WiFi management for the Jettyd firmware SDK.
 *
 * Handles WiFi connection with retry and exponential backoff.
 */

#ifndef JETTYD_WIFI_H
#define JETTYD_WIFI_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/** Maximum retry attempts before giving up (0 = infinite) */
#define JETTYD_WIFI_MAX_RETRIES     0

/** Initial backoff delay in milliseconds */
#define JETTYD_WIFI_BACKOFF_INIT_MS 1000

/** Maximum backoff delay in milliseconds */
#define JETTYD_WIFI_BACKOFF_MAX_MS  60000

/**
 * @brief WiFi connection state.
 */
typedef enum {
    JETTYD_WIFI_DISCONNECTED,
    JETTYD_WIFI_CONNECTING,
    JETTYD_WIFI_CONNECTED,
    JETTYD_WIFI_FAILED,
} jettyd_wifi_state_t;

/**
 * @brief Initialize the WiFi subsystem.
 *
 * Configures the WiFi driver in station mode. Does not connect.
 */
esp_err_t jettyd_wifi_init(void);

/**
 * @brief Connect to WiFi using credentials from NVS.
 *
 * Uses exponential backoff on failure. Blocks until connected
 * or max retries exceeded.
 *
 * @return ESP_OK when connected.
 */
esp_err_t jettyd_wifi_connect(void);

/**
 * @brief Connect with explicit credentials.
 *
 * @param ssid     WiFi SSID.
 * @param password WiFi password.
 */
esp_err_t jettyd_wifi_connect_with(const char *ssid, const char *password);

/**
 * @brief Disconnect from WiFi.
 */
esp_err_t jettyd_wifi_disconnect(void);

/**
 * @brief Get current WiFi state.
 */
jettyd_wifi_state_t jettyd_wifi_get_state(void);

/**
 * @brief Get current RSSI (signal strength).
 *
 * @return RSSI in dBm, or 0 if not connected.
 */
int8_t jettyd_wifi_get_rssi(void);

/**
 * @brief Check if WiFi is connected.
 */
bool jettyd_wifi_is_connected(void);

#endif /* JETTYD_WIFI_H */
