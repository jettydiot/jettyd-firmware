/**
 * @file telemetry.c
 * @brief Telemetry publishing: reads drivers, builds JSON, publishes.
 */

#include "jettyd_telemetry.h"
#include "jettyd_mqtt.h"
#include "jettyd_shadow.h"
#include "jettyd_wifi.h"
#include "jettyd.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "jettyd_telem";

static const jettyd_config_t *s_config = NULL;

esp_err_t jettyd_telemetry_init(void)
{
    s_config = jettyd_get_config();
    ESP_LOGI(TAG, "Telemetry initialized");
    return ESP_OK;
}

void jettyd_telemetry_get_system_metrics(float *battery, int8_t *rssi,
                                          uint32_t *uptime, uint32_t *heap_free)
{
    if (rssi) {
        *rssi = jettyd_wifi_get_rssi();
    }
    if (uptime) {
        *uptime = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    }
    if (heap_free) {
        *heap_free = (uint32_t)esp_get_free_heap_size();
    }
    if (battery) {
        *battery = 0.0f;
        /* Battery reading via ADC if configured */
        if (s_config && s_config->has_battery && s_config->battery_adc_pin >= 0) {
            /* ADC read would go here — simplified: report 0 if no ADC driver */
            *battery = 0.0f;
        }
    }
}

/**
 * @brief Read a single dotted metric and add it to a cJSON object.
 *
 * For "system.*" metrics, reads system state directly.
 * For driver metrics, reads via the driver registry.
 */
static void add_metric_to_json(cJSON *readings, const char *dotted_name)
{
    /* Handle system metrics */
    if (strncmp(dotted_name, "system.", 7) == 0) {
        const char *sys_key = dotted_name + 7;
        float battery;
        int8_t rssi;
        uint32_t uptime_val, heap;
        jettyd_telemetry_get_system_metrics(&battery, &rssi, &uptime_val, &heap);

        if (strcmp(sys_key, "battery") == 0) {
            cJSON_AddNumberToObject(readings, dotted_name, battery);
        } else if (strcmp(sys_key, "rssi") == 0) {
            cJSON_AddNumberToObject(readings, dotted_name, rssi);
        } else if (strcmp(sys_key, "uptime") == 0) {
            cJSON_AddNumberToObject(readings, dotted_name, uptime_val);
        } else if (strcmp(sys_key, "heap_free") == 0) {
            cJSON_AddNumberToObject(readings, dotted_name, heap);
        }
        return;
    }

    /* Driver metric: parse "instance.capability" */
    const jettyd_driver_t *drv = jettyd_driver_find_capability(dotted_name);
    if (drv == NULL || drv->read == NULL) {
        return;
    }

    const char *dot = strchr(dotted_name, '.');
    if (dot == NULL) return;
    const char *cap_name = dot + 1;

    jettyd_value_t val = drv->read(cap_name);
    if (!val.valid) return;

    /* Also update shadow */
    jettyd_shadow_update(dotted_name, val);

    switch (val.type) {
    case JETTYD_VAL_FLOAT:
        cJSON_AddNumberToObject(readings, dotted_name, val.float_val);
        break;
    case JETTYD_VAL_INT:
        cJSON_AddNumberToObject(readings, dotted_name, val.int_val);
        break;
    case JETTYD_VAL_BOOL:
        cJSON_AddBoolToObject(readings, dotted_name, val.bool_val);
        break;
    case JETTYD_VAL_STRING:
        cJSON_AddStringToObject(readings, dotted_name, val.str_val);
        break;
    }
}

esp_err_t jettyd_telemetry_publish(const char **metrics, uint8_t metric_count)
{
    if (metric_count == 0) {
        return jettyd_telemetry_publish_all();
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "ts", (double)time(NULL));

    cJSON *readings = cJSON_CreateObject();

    for (uint8_t i = 0; i < metric_count; i++) {
        if (metrics[i]) {
            add_metric_to_json(readings, metrics[i]);
        }
    }

    /* Always include system metrics */
    add_metric_to_json(readings, "system.battery");
    add_metric_to_json(readings, "system.rssi");
    add_metric_to_json(readings, "system.uptime");
    add_metric_to_json(readings, "system.heap_free");

    cJSON_AddItemToObject(root, "readings", readings);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char topic[JETTYD_MQTT_MAX_TOPIC];
    esp_err_t err = jettyd_mqtt_build_topic(topic, sizeof(topic), "telemetry");
    if (err != ESP_OK) {
        cJSON_free(json_str);
        return err;
    }

    err = jettyd_mqtt_publish(topic, json_str, 1, false);
    cJSON_free(json_str);

    if (err == ESP_OK) {
        ESP_LOGD(TAG, "Published telemetry (%d metrics)", metric_count);
    }
    return err;
}

esp_err_t jettyd_telemetry_publish_all(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "ts", (double)time(NULL));

    cJSON *readings = cJSON_CreateObject();

    /* Read all driver capabilities */
    uint8_t count = jettyd_driver_count();
    for (uint8_t d = 0; d < count; d++) {
        const jettyd_driver_t *drv = jettyd_driver_get(d);
        if (drv == NULL || drv->read == NULL) continue;

        for (uint8_t c = 0; c < drv->capability_count; c++) {
            if (drv->capabilities[c].type != JETTYD_CAP_READABLE &&
                drv->capabilities[c].type != JETTYD_CAP_SWITCHABLE) {
                continue;
            }

            char dotted[48];
            snprintf(dotted, sizeof(dotted), "%s.%s",
                     drv->instance, drv->capabilities[c].name);

            if (drv->capabilities[c].type == JETTYD_CAP_SWITCHABLE && drv->get_state) {
                jettyd_value_t val = {
                    .type = JETTYD_VAL_BOOL,
                    .bool_val = drv->get_state(),
                    .valid = true
                };
                cJSON_AddBoolToObject(readings, dotted, val.bool_val);
                jettyd_shadow_update(dotted, val);
            } else {
                add_metric_to_json(readings, dotted);
            }
        }
    }

    /* System metrics */
    add_metric_to_json(readings, "system.battery");
    add_metric_to_json(readings, "system.rssi");
    add_metric_to_json(readings, "system.uptime");
    add_metric_to_json(readings, "system.heap_free");

    cJSON_AddItemToObject(root, "readings", readings);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char topic[JETTYD_MQTT_MAX_TOPIC];
    esp_err_t err = jettyd_mqtt_build_topic(topic, sizeof(topic), "telemetry");
    if (err != ESP_OK) {
        cJSON_free(json_str);
        return err;
    }

    err = jettyd_mqtt_publish(topic, json_str, 1, false);
    cJSON_free(json_str);
    return err;
}

esp_err_t jettyd_telemetry_publish_alert(const char *message, const char *severity)
{
    if (message == NULL || severity == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "ts", (double)time(NULL));
    cJSON_AddStringToObject(root, "message", message);
    cJSON_AddStringToObject(root, "severity", severity);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char topic[JETTYD_MQTT_MAX_TOPIC];
    esp_err_t err = jettyd_mqtt_build_topic(topic, sizeof(topic), "alert");
    if (err != ESP_OK) {
        cJSON_free(json_str);
        return err;
    }

    err = jettyd_mqtt_publish(topic, json_str, 1, false);
    cJSON_free(json_str);
    return err;
}

esp_err_t jettyd_telemetry_publish_status(const char *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", status);
    cJSON_AddNumberToObject(root, "ts", (double)time(NULL));

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char topic[JETTYD_MQTT_MAX_TOPIC];
    esp_err_t err = jettyd_mqtt_build_topic(topic, sizeof(topic), "status");
    if (err != ESP_OK) {
        cJSON_free(json_str);
        return err;
    }

    err = jettyd_mqtt_publish(topic, json_str, 1, false);
    cJSON_free(json_str);
    return err;
}
