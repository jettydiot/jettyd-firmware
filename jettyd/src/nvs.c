/**
 * @file nvs.c
 * @brief NVS abstraction implementation.
 */

#include "jettyd_nvs.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <stdbool.h>

static const char *TAG = "jettyd_nvs";

esp_err_t jettyd_nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition corrupt or version mismatch, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t jettyd_nvs_read_str(const char *ns, const char *key, char *buf, size_t buf_len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t required = buf_len;
    err = nvs_get_str(handle, key, buf, &required);
    nvs_close(handle);
    return err;
}

esp_err_t jettyd_nvs_write_str(const char *ns, const char *key, const char *value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t jettyd_nvs_read_blob(const char *ns, const char *key, void *buf, size_t *len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_blob(handle, key, buf, len);
    nvs_close(handle);
    return err;
}

esp_err_t jettyd_nvs_write_blob(const char *ns, const char *key, const void *data, size_t len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(handle, key, data, len);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t jettyd_nvs_read_u32(const char *ns, const char *key, uint32_t *value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_u32(handle, key, value);
    nvs_close(handle);
    return err;
}

esp_err_t jettyd_nvs_write_u32(const char *ns, const char *key, uint32_t value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u32(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t jettyd_nvs_read_u8(const char *ns, const char *key, uint8_t *value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_u8(handle, key, value);
    nvs_close(handle);
    return err;
}

esp_err_t jettyd_nvs_write_u8(const char *ns, const char *key, uint8_t value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t jettyd_nvs_erase_key(const char *ns, const char *key)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_erase_key(handle, key);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t jettyd_nvs_erase_namespace(const char *ns)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

bool jettyd_nvs_key_exists(const char *ns, const char *key)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }

    /* Try to get the size of the string value without reading it */
    size_t len = 0;
    err = nvs_get_str(handle, key, NULL, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* Try blob */
        err = nvs_get_blob(handle, key, NULL, &len);
    }
    nvs_close(handle);
    return (err == ESP_OK);
}
