/**
 * @file jettyd_nvs.h
 * @brief NVS abstraction for the Jettyd firmware SDK.
 *
 * Wraps ESP-IDF NVS operations with Jettyd-specific namespaces
 * and error handling.
 */

#ifndef JETTYD_NVS_H
#define JETTYD_NVS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/** NVS namespaces */
#define JETTYD_NVS_NS_PROV     "jettyd_prov"    /**< Provisioning: 4KB */
#define JETTYD_NVS_NS_VM       "jettyd_vm"      /**< JettyScript config: 4KB */
#define JETTYD_NVS_NS_SHADOW   "jettyd_shadow"  /**< Last shadow state: 2KB */
#define JETTYD_NVS_NS_OTA      "jettyd_ota"     /**< OTA state: 1KB */

/**
 * @brief Initialize NVS flash.
 *
 * Calls nvs_flash_init(). If NVS is corrupt or has no free pages,
 * erases and re-initializes.
 */
esp_err_t jettyd_nvs_init(void);

/**
 * @brief Read a string from NVS.
 *
 * @param ns      Namespace.
 * @param key     Key name.
 * @param buf     Output buffer.
 * @param buf_len Buffer size.
 * @return ESP_OK on success, ESP_ERR_NVS_NOT_FOUND if key absent.
 */
esp_err_t jettyd_nvs_read_str(const char *ns, const char *key, char *buf, size_t buf_len);

/**
 * @brief Write a string to NVS.
 */
esp_err_t jettyd_nvs_write_str(const char *ns, const char *key, const char *value);

/**
 * @brief Read a blob from NVS.
 */
esp_err_t jettyd_nvs_read_blob(const char *ns, const char *key, void *buf, size_t *len);

/**
 * @brief Write a blob to NVS.
 */
esp_err_t jettyd_nvs_write_blob(const char *ns, const char *key, const void *data, size_t len);

/**
 * @brief Read a uint32 from NVS.
 */
esp_err_t jettyd_nvs_read_u32(const char *ns, const char *key, uint32_t *value);

/**
 * @brief Write a uint32 to NVS.
 */
esp_err_t jettyd_nvs_write_u32(const char *ns, const char *key, uint32_t value);

/**
 * @brief Read a uint8 from NVS.
 */
esp_err_t jettyd_nvs_read_u8(const char *ns, const char *key, uint8_t *value);

/**
 * @brief Write a uint8 to NVS.
 */
esp_err_t jettyd_nvs_write_u8(const char *ns, const char *key, uint8_t value);

/**
 * @brief Delete a key from NVS.
 */
esp_err_t jettyd_nvs_erase_key(const char *ns, const char *key);

/**
 * @brief Erase all keys in a namespace.
 */
esp_err_t jettyd_nvs_erase_namespace(const char *ns);

/**
 * @brief Check if a key exists in NVS.
 */
bool jettyd_nvs_key_exists(const char *ns, const char *key);

#endif /* JETTYD_NVS_H */
