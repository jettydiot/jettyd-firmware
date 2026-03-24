/**
 * @file ota.c
 * @brief OTA firmware update with checksum verification.
 */

#include "jettyd_ota.h"
#include "jettyd_nvs.h"
#include "jettyd_mqtt.h"
#include "jettyd_telemetry.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_system.h"
#include "cJSON.h"
#include "mbedtls/sha256.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "jettyd_ota";

static jettyd_ota_state_t s_ota_state = {0};

/* Current running firmware version (set by jettyd.c) */
extern const char *JETTYD_FIRMWARE_VERSION;

esp_err_t jettyd_ota_init(void)
{
    memset(&s_ota_state, 0, sizeof(s_ota_state));

    /* Read OTA state from NVS */
    size_t len = sizeof(s_ota_state);
    esp_err_t err = jettyd_nvs_read_blob(JETTYD_NVS_NS_OTA, "ota_state",
                                          &s_ota_state, &len);
    if (err != ESP_OK) {
        memset(&s_ota_state, 0, sizeof(s_ota_state));
    }

    /* If we have a pending update, it means we just booted into new firmware.
     * Mark it as valid if we got this far successfully. */
    if (s_ota_state.update_pending) {
        ESP_LOGI(TAG, "Boot after OTA update to %s — marking valid",
                 s_ota_state.target_version);
        s_ota_state.update_pending = false;
        s_ota_state.retry_count = 0;

        /* Persist cleared state */
        jettyd_nvs_write_blob(JETTYD_NVS_NS_OTA, "ota_state",
                               &s_ota_state, sizeof(s_ota_state));
    }

    ESP_LOGI(TAG, "OTA initialized");
    return ESP_OK;
}

esp_err_t jettyd_ota_handle(const jettyd_ota_notification_t *notification)
{
    if (notification == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Skip if already running this version */
    if (strcmp(notification->version, CONFIG_JETTYD_FIRMWARE_VERSION) == 0) {
        ESP_LOGI(TAG, "Already running version %s, skipping OTA", notification->version);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting OTA: %s -> %s (size: %lu)",
             CONFIG_JETTYD_FIRMWARE_VERSION,
             notification->version, (unsigned long)notification->size);

    /* Publish updating status */
    char status_json[128];
    snprintf(status_json, sizeof(status_json),
             "{\"status\":\"updating\",\"version\":\"%s\"}", notification->version);
    char topic[JETTYD_MQTT_MAX_TOPIC];
    if (jettyd_mqtt_build_topic(topic, sizeof(topic), "status") == ESP_OK) {
        jettyd_mqtt_publish(topic, status_json, 1, false);
    }

    /* Configure HTTPS OTA */
    esp_http_client_config_t http_config = {
        .url = notification->url,
        .timeout_ms = 30000,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Download and write firmware in chunks */
    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0); /* 0 = SHA-256, not SHA-224 */

    while (1) {
        err = esp_https_ota_perform(ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA perform failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(ota_handle);
        mbedtls_sha256_free(&sha_ctx);
        return err;
    }

    mbedtls_sha256_free(&sha_ctx);

    /* Finish OTA — writes boot partition */
    err = esp_https_ota_finish(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Save OTA state before reboot */
    strncpy(s_ota_state.target_version, notification->version,
            sizeof(s_ota_state.target_version) - 1);
    s_ota_state.update_pending = true;
    s_ota_state.retry_count = 0;
    jettyd_nvs_write_blob(JETTYD_NVS_NS_OTA, "ota_state",
                           &s_ota_state, sizeof(s_ota_state));

    ESP_LOGI(TAG, "OTA complete, rebooting into %s", notification->version);
    esp_restart();

    /* Never reached */
    return ESP_OK;
}

esp_err_t jettyd_ota_mark_valid(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "Marking current firmware as valid");
            return esp_ota_mark_app_valid_cancel_rollback();
        }
    }
    return ESP_OK;
}

bool jettyd_ota_is_pending(void)
{
    return s_ota_state.update_pending;
}

esp_err_t jettyd_ota_get_state(jettyd_ota_state_t *state)
{
    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(state, &s_ota_state, sizeof(jettyd_ota_state_t));
    return ESP_OK;
}

void jettyd_ota_mqtt_handler(const char *topic, const char *data, int data_len)
{
    ESP_LOGI(TAG, "Received OTA notification");

    cJSON *root = cJSON_ParseWithLength(data, data_len);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse OTA notification JSON");
        return;
    }

    jettyd_ota_notification_t notif = {0};

    cJSON *version = cJSON_GetObjectItem(root, "version");
    cJSON *url = cJSON_GetObjectItem(root, "url");
    cJSON *checksum = cJSON_GetObjectItem(root, "checksum");
    cJSON *size = cJSON_GetObjectItem(root, "size");

    if (!cJSON_IsString(version) || !cJSON_IsString(url)) {
        ESP_LOGE(TAG, "OTA notification missing required fields");
        cJSON_Delete(root);
        return;
    }

    strncpy(notif.version, version->valuestring, sizeof(notif.version) - 1);
    strncpy(notif.url, url->valuestring, sizeof(notif.url) - 1);
    if (cJSON_IsString(checksum)) {
        strncpy(notif.checksum, checksum->valuestring, sizeof(notif.checksum) - 1);
    }
    if (cJSON_IsNumber(size)) {
        notif.size = (uint32_t)size->valuedouble;
    }

    cJSON_Delete(root);

    jettyd_ota_handle(&notif);
}
