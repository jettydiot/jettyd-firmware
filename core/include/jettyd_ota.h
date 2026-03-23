/**
 * @file jettyd_ota.h
 * @brief OTA update interface for the Jettyd firmware SDK.
 *
 * Handles firmware download, checksum verification, partition write,
 * and rollback on failed boot.
 */

#ifndef JETTYD_OTA_H
#define JETTYD_OTA_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/**
 * @brief OTA update notification payload (from MQTT firmware topic).
 */
typedef struct {
    char version[32];           /**< Target firmware version */
    char url[256];              /**< HTTPS URL to firmware binary */
    char checksum[72];          /**< "sha256:<hex>" */
    uint32_t size;              /**< Expected binary size in bytes */
} jettyd_ota_notification_t;

/**
 * @brief OTA state persisted in NVS across reboots.
 */
typedef struct {
    char target_version[32];    /**< Version we're trying to update to */
    uint8_t retry_count;        /**< Number of failed attempts */
    bool update_pending;        /**< True if update was written but not yet verified */
} jettyd_ota_state_t;

/**
 * @brief Initialize the OTA subsystem.
 *
 * Reads OTA state from NVS. If a pending update exists and the current
 * boot is successful, marks the partition as valid. If the boot fails
 * (detected by watchdog rollback), ESP-IDF automatically reverts.
 */
esp_err_t jettyd_ota_init(void);

/**
 * @brief Handle an OTA update notification from the firmware topic.
 *
 * Compares version to current, downloads the binary, verifies checksum,
 * writes to the OTA partition, and reboots.
 *
 * @param notification Parsed OTA notification.
 * @return ESP_OK if update started (device will reboot), or error code.
 */
esp_err_t jettyd_ota_handle(const jettyd_ota_notification_t *notification);

/**
 * @brief Mark the current firmware as valid after successful boot.
 *
 * Called after the device successfully connects and starts the VM.
 * Prevents rollback to the previous firmware.
 */
esp_err_t jettyd_ota_mark_valid(void);

/**
 * @brief Check if an OTA update is in progress.
 */
bool jettyd_ota_is_pending(void);

/**
 * @brief Get the current OTA state from NVS.
 */
esp_err_t jettyd_ota_get_state(jettyd_ota_state_t *state);

/**
 * @brief MQTT callback for the firmware topic.
 *
 * Parses the JSON notification and calls jettyd_ota_handle().
 */
void jettyd_ota_mqtt_handler(const char *topic, const char *data, int data_len);

#endif /* JETTYD_OTA_H */
