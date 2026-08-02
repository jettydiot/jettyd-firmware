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
 * Applies the given credentials and initiates the association. This does not
 * block until connected — pair it with jettyd_wifi_wait_connected() to wait
 * for a result. Used both at boot and by jettyd_wifi_reconfigure().
 *
 * @param ssid     WiFi SSID.
 * @param password WiFi password.
 */
esp_err_t jettyd_wifi_connect_with(const char *ssid, const char *password);

/**
 * @brief Block until WiFi reaches JETTYD_WIFI_CONNECTED or the timeout elapses.
 *
 * @param timeout_ms Maximum time to wait, in milliseconds. 0 waits forever.
 * @return ESP_OK if connected, ESP_ERR_TIMEOUT if the timeout elapsed first.
 */
esp_err_t jettyd_wifi_wait_connected(uint32_t timeout_ms);

/**
 * @brief Update WiFi credentials at runtime and reconnect, with rollback.
 *
 * Holds the current credentials in RAM, writes the new SSID/password to the
 * provisioning NVS keys (wifi_ssid / wifi_pass in namespace jettyd_prov),
 * disconnects and reconnects to the new network. If the new network does not
 * reach JETTYD_WIFI_CONNECTED within @p timeout_ms, the previous credentials
 * are restored to NVS and the device reconnects to the previous network.
 *
 * The provisioning identity keys (device_key, fleet_token, tenant_id) are
 * never touched. An empty or NULL password means an open network.
 *
 * @param ssid       New SSID (1..32 bytes).
 * @param password   New password (0..64 bytes); empty/NULL for an open network.
 * @param timeout_ms Time to wait for the new network before rolling back.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG / ESP_ERR_INVALID_SIZE for an
 *         invalid payload (no NVS write), ESP_FAIL if the switch failed and the
 *         previous network was restored.
 */
esp_err_t jettyd_wifi_reconfigure(const char *ssid, const char *password, uint32_t timeout_ms);

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

/**
 * @brief Return the number of consecutive station connection failures.
 *
 * Resets to 0 on a successful IP assignment. Used by the portal trigger to
 * decide whether to summon the config AP. Returns 0 on non-WiFi targets.
 */
int jettyd_wifi_get_fail_count(void);

#endif /* JETTYD_WIFI_H */
