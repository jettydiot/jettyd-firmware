/**
 * @file jettyd_provision.h
 * @brief Fleet provisioning interface for the Jettyd firmware SDK.
 *
 * A new device connects with a fleet token, publishes a provisioning
 * request, and receives a device key. The key is stored in NVS and
 * used for all subsequent connections.
 */

#ifndef JETTYD_PROVISION_H
#define JETTYD_PROVISION_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/** NVS namespace for provisioning data */
#define JETTYD_PROV_NVS_NAMESPACE   "jettyd_prov"

/** NVS keys */
#define JETTYD_PROV_KEY_TENANT_ID   "tenant_id"
#define JETTYD_PROV_KEY_DEVICE_ID   "device_id"
#define JETTYD_PROV_KEY_DEVICE_KEY  "device_key"
#define JETTYD_PROV_KEY_FLEET_TOKEN "fleet_token"
#define JETTYD_PROV_KEY_MQTT_URI    "mqtt_uri"
#define JETTYD_PROV_KEY_WIFI_SSID   "wifi_ssid"
#define JETTYD_PROV_KEY_WIFI_PASS   "wifi_pass"

/**
 * @brief Provisioning state.
 */
typedef struct {
    char tenant_id[64];
    char device_id[64];
    char device_key[128];
    char fleet_token[128];
    char mqtt_uri[128];
    char wifi_ssid[33];     /**< Max 32 chars + null */
    char wifi_pass[65];     /**< Max 64 chars + null */
    bool provisioned;       /**< True if device_key is present */
} jettyd_provision_state_t;

/**
 * @brief Initialize the provisioning subsystem.
 *
 * Reads provisioning state from NVS.
 */
esp_err_t jettyd_provision_init(void);

/**
 * @brief Check if the device is already provisioned.
 */
bool jettyd_provision_is_provisioned(void);

/**
 * @brief Get the current provisioning state.
 */
const jettyd_provision_state_t *jettyd_provision_get_state(void);

/**
 * @brief Run the provisioning flow.
 *
 * 1. Connect to MQTT with fleet token
 * 2. Publish to jettyd/provision/request
 * 3. Wait for response on jettyd/provision/response/{device_key}
 * 4. Store device key in NVS
 * 5. Delete fleet token from NVS
 *
 * @return ESP_OK on success, error code on failure.
 */
esp_err_t jettyd_provision_run(void);

/**
 * @brief Store provisioning credentials in NVS.
 *
 * Called during initial flashing (tools/flash.py writes these to NVS).
 */
esp_err_t jettyd_provision_store(const jettyd_provision_state_t *state);

/**
 * @brief Clear all provisioning data from NVS.
 *
 * Used for factory reset.
 */
esp_err_t jettyd_provision_clear(void);

#endif /* JETTYD_PROVISION_H */
