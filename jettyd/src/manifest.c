/**
 * @file manifest.c
 * @brief Driver capability manifest publisher (FLU-113).
 *
 * Publishes a retained QoS-1 JSON manifest to
 *   jettyd/{tenant_id}/{device_key}/manifest
 * at boot and on every MQTT reconnect.
 */

#include "jettyd_manifest.h"
#include "jettyd_driver.h"
#include "jettyd_mqtt.h"
#include "jettyd_provision.h"
#include "jettyd.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

static const char *TAG = "jettyd_manifest";

/* Maximum JSON payload size for the manifest */
#define MANIFEST_BUF_SIZE 512

/* ───────────────────────────── Helpers ────────────────────────────────────── */

/**
 * @brief Return true if the driver instance name is safe to include.
 *
 * Allowed characters: [A-Za-z0-9_].  Empty names are rejected.
 */
static bool name_is_safe(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    for (const char *p = name; *p != '\0'; p++) {
        char c = *p;
        bool alnum = (c >= 'A' && c <= 'Z') ||
                     (c >= 'a' && c <= 'z') ||
                     (c >= '0' && c <= '9');
        bool underscore = (c == '_');
        if (!alnum && !underscore) {
            return false;
        }
    }
    return true;
}

/* ───────────────────────────── Public API ─────────────────────────────────── */

esp_err_t jettyd_publish_manifest(void)
{
    /* Require an active MQTT connection and provisioned state */
    const jettyd_provision_state_t *prov = jettyd_provision_get_state();
    if (prov == NULL || !prov->provisioned) {
        ESP_LOGD(TAG, "Not provisioned — skipping manifest publish");
        return ESP_OK;
    }

    if (!jettyd_mqtt_is_connected()) {
        ESP_LOGD(TAG, "MQTT not connected — skipping manifest publish");
        return ESP_OK;
    }

    /* Build topic */
    char topic[JETTYD_MQTT_MAX_TOPIC];
    esp_err_t err = jettyd_mqtt_build_topic(topic, sizeof(topic), "manifest");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to build manifest topic");
        return err;
    }

    /* Determine firmware version */
    const char *fw_version = JETTYD_FIRMWARE_VERSION;
    if (fw_version == NULL || fw_version[0] == '\0') {
        fw_version = "0.0.0";
    }

    /* Determine chip string */
#ifdef CONFIG_IDF_TARGET
    const char *chip = CONFIG_IDF_TARGET;
#elif defined(CONFIG_JETTYD_DEVICE_TYPE)
    const char *chip = CONFIG_JETTYD_DEVICE_TYPE;
#else
    const char *chip = "esp32";
#endif

    /* Build JSON payload manually (avoids cJSON heap dependency for a small, */
    /* fixed-schema document and keeps the function self-contained).           */
    char buf[MANIFEST_BUF_SIZE];
    int pos = 0;
    int remaining = (int)sizeof(buf);

    /* Open object and start drivers array */
    int written = snprintf(buf + pos, (size_t)remaining, "{\"drivers\":[");
    if (written < 0 || written >= remaining) {
        return ESP_ERR_NO_MEM;
    }
    pos += written;
    remaining -= written;

    /* Append sanitized driver instance names */
    uint8_t count = jettyd_driver_count();
    bool first = true;
    for (uint8_t i = 0; i < count; i++) {
        const jettyd_driver_t *drv = jettyd_driver_get(i);
        if (drv == NULL) {
            continue;
        }
        if (!name_is_safe(drv->instance)) {
            ESP_LOGW(TAG, "Skipping driver with unsafe instance name: '%s'", drv->instance);
            continue;
        }
        written = snprintf(buf + pos, (size_t)remaining,
                           "%s\"%s\"", first ? "" : ",", drv->instance);
        if (written < 0 || written >= remaining) {
            return ESP_ERR_NO_MEM;
        }
        pos += written;
        remaining -= written;
        first = false;
    }

    /* Close drivers array and append firmware + chip */
    written = snprintf(buf + pos, (size_t)remaining,
                       "],\"firmware\":\"%s\",\"chip\":\"%s\"}",
                       fw_version, chip);
    if (written < 0 || written >= remaining) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Publishing manifest: %s", buf);

    err = jettyd_mqtt_publish(topic, buf, /*qos=*/1, /*retain=*/true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to publish manifest (err=%d)", err);
    }
    return err;
}
