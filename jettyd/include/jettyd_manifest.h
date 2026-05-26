/**
 * @file jettyd_manifest.h
 * @brief Driver capability manifest publisher (FLU-113).
 *
 * Publishes a retained QoS-1 JSON manifest to
 * `jettyd/{tenant_id}/{device_key}/manifest` describing all registered
 * drivers and firmware metadata.  Called once at the end of the boot
 * sequence and again on every MQTT reconnect so that a broker restart
 * cannot permanently lose the retained message.
 *
 * Example payload:
 * @code
 * {"drivers":["soil","valve"],"firmware":"1.0.0","chip":"esp32s3"}
 * @endcode
 */

#ifndef JETTYD_MANIFEST_H
#define JETTYD_MANIFEST_H

#include "esp_err.h"

/**
 * @brief Publish the driver capability manifest to MQTT.
 *
 * Builds a JSON object with:
 *  - "drivers": array of registered driver instance names (sanitized —
 *    names containing characters other than [A-Za-z0-9_] are skipped).
 *  - "firmware": firmware version string from CONFIG_JETTYD_FIRMWARE_VERSION
 *    (falls back to JETTYD_FIRMWARE_VERSION global).
 *  - "chip": target chip ID from CONFIG_IDF_TARGET (falls back to
 *    CONFIG_JETTYD_DEVICE_TYPE or "esp32").
 *
 * The message is published with retain=true and QoS 1.  If the device is
 * not yet connected to MQTT the call is a no-op and returns ESP_OK — the
 * publish will happen on the next reconnect event instead.
 *
 * @return ESP_OK on success or when skipped (not connected / not provisioned).
 *         ESP_ERR_NO_MEM if the JSON buffer is too small.
 */
esp_err_t jettyd_publish_manifest(void);

#endif /* JETTYD_MANIFEST_H */
