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
#include <stdbool.h>
#include "esp_system.h"
#include "esp_timer.h"
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

static int append_value_to_buf(char *buf, size_t buf_len, int pos,
                               const char *key, jettyd_value_t val, bool *first)
{
    if (!*first) {
        pos += snprintf(buf + pos, buf_len - pos, ",");
    }
    *first = false;

    switch (val.type) {
    case JETTYD_VAL_FLOAT:
        pos += snprintf(buf + pos, buf_len - pos, "\"%s\":%g", key, (double)val.float_val);
        break;
    case JETTYD_VAL_INT:
        pos += snprintf(buf + pos, buf_len - pos, "\"%s\":%ld", key, (long)val.int_val);
        break;
    case JETTYD_VAL_BOOL:
        pos += snprintf(buf + pos, buf_len - pos, "\"%s\":%s", key,
                        val.bool_val ? "true" : "false");
        break;
    case JETTYD_VAL_STRING:
        pos += snprintf(buf + pos, buf_len - pos, "\"%s\":\"%s\"", key, val.str_val);
        break;
    }
    return pos;
}

static int add_metric_to_buf(char *buf, size_t buf_len, int pos,
                             const char *dotted_name, bool *first)
{
    /* Handle system metrics */
    if (strncmp(dotted_name, "system.", 7) == 0) {
        const char *sys_key = dotted_name + 7;
        float battery;
        int8_t rssi;
        uint32_t uptime_val, heap;
        jettyd_telemetry_get_system_metrics(&battery, &rssi, &uptime_val, &heap);

        jettyd_value_t val = {.valid = true};
        if (strcmp(sys_key, "battery") == 0) {
            val.type = JETTYD_VAL_FLOAT;
            val.float_val = battery;
        } else if (strcmp(sys_key, "rssi") == 0) {
            val.type = JETTYD_VAL_INT;
            val.int_val = rssi;
        } else if (strcmp(sys_key, "uptime") == 0) {
            val.type = JETTYD_VAL_INT;
            val.int_val = (int32_t)uptime_val;
        } else if (strcmp(sys_key, "heap_free") == 0) {
            val.type = JETTYD_VAL_INT;
            val.int_val = (int32_t)heap;
        } else {
            return pos;
        }
        return append_value_to_buf(buf, buf_len, pos, dotted_name, val, first);
    }

    /* Driver metric: parse "instance.capability" */
    const jettyd_driver_t *drv = jettyd_driver_find_capability(dotted_name);
    if (drv == NULL || drv->read == NULL) {
        return pos;
    }

    const char *dot = strchr(dotted_name, '.');
    if (dot == NULL) return pos;
    const char *cap_name = dot + 1;

    jettyd_value_t val = drv->read(cap_name);
    if (!val.valid) return pos;

    /* Also update shadow */
    jettyd_shadow_update(dotted_name, val);

    return append_value_to_buf(buf, buf_len, pos, dotted_name, val, first);
}

esp_err_t jettyd_telemetry_publish(const char **metrics, uint8_t metric_count)
{
    if (metric_count == 0) {
        return jettyd_telemetry_publish_all();
    }

    static char telemetry_buf[512];
    int pos = 0;
    bool first = true;

    pos += snprintf(telemetry_buf + pos, sizeof(telemetry_buf) - pos,
                    "{\"ts\":%lld,\"readings\":{", (long long)time(NULL));

    for (uint8_t i = 0; i < metric_count; i++) {
        if (metrics[i]) {
            pos = add_metric_to_buf(telemetry_buf, sizeof(telemetry_buf), pos,
                                    metrics[i], &first);
        }
    }

    /* Always include system metrics */
    pos = add_metric_to_buf(telemetry_buf, sizeof(telemetry_buf), pos,
                            "system.battery", &first);
    pos = add_metric_to_buf(telemetry_buf, sizeof(telemetry_buf), pos,
                            "system.rssi", &first);
    pos = add_metric_to_buf(telemetry_buf, sizeof(telemetry_buf), pos,
                            "system.uptime", &first);
    pos = add_metric_to_buf(telemetry_buf, sizeof(telemetry_buf), pos,
                            "system.heap_free", &first);

    pos += snprintf(telemetry_buf + pos, sizeof(telemetry_buf) - pos, "}}");

    if ((size_t)pos >= sizeof(telemetry_buf)) {
        return ESP_ERR_NO_MEM;
    }

    char topic[JETTYD_MQTT_MAX_TOPIC];
    esp_err_t err = jettyd_mqtt_build_topic(topic, sizeof(topic), "telemetry");
    if (err != ESP_OK) {
        return err;
    }

    err = jettyd_mqtt_publish(topic, telemetry_buf, 1, false);
    if (err == ESP_OK) {
        ESP_LOGD(TAG, "Published telemetry (%d metrics)", metric_count);
    }
    return err;
}

esp_err_t jettyd_telemetry_publish_all(void)
{
    static char telemetry_buf[512];
    int pos = 0;
    bool first = true;

    pos += snprintf(telemetry_buf + pos, sizeof(telemetry_buf) - pos,
                    "{\"ts\":%lld,\"readings\":{", (long long)time(NULL));

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
                pos = append_value_to_buf(telemetry_buf, sizeof(telemetry_buf),
                                          pos, dotted, val, &first);
                jettyd_shadow_update(dotted, val);
            } else {
                pos = add_metric_to_buf(telemetry_buf, sizeof(telemetry_buf),
                                        pos, dotted, &first);
            }
        }
    }

    /* System metrics */
    pos = add_metric_to_buf(telemetry_buf, sizeof(telemetry_buf), pos,
                            "system.battery", &first);
    pos = add_metric_to_buf(telemetry_buf, sizeof(telemetry_buf), pos,
                            "system.rssi", &first);
    pos = add_metric_to_buf(telemetry_buf, sizeof(telemetry_buf), pos,
                            "system.uptime", &first);
    pos = add_metric_to_buf(telemetry_buf, sizeof(telemetry_buf), pos,
                            "system.heap_free", &first);

    pos += snprintf(telemetry_buf + pos, sizeof(telemetry_buf) - pos, "}}");

    if ((size_t)pos >= sizeof(telemetry_buf)) {
        return ESP_ERR_NO_MEM;
    }

    char topic[JETTYD_MQTT_MAX_TOPIC];
    esp_err_t err = jettyd_mqtt_build_topic(topic, sizeof(topic), "telemetry");
    if (err != ESP_OK) {
        return err;
    }

    return jettyd_mqtt_publish(topic, telemetry_buf, 1, false);
}

esp_err_t jettyd_telemetry_publish_alert(const char *message, const char *severity)
{
    if (message == NULL || severity == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    static char alert_buf[256];
    snprintf(alert_buf, sizeof(alert_buf),
             "{\"ts\":%lld,\"message\":\"%s\",\"severity\":\"%s\"}",
             (long long)time(NULL), message, severity);

    char topic[JETTYD_MQTT_MAX_TOPIC];
    esp_err_t err = jettyd_mqtt_build_topic(topic, sizeof(topic), "alert");
    if (err != ESP_OK) {
        return err;
    }

    return jettyd_mqtt_publish(topic, alert_buf, 1, false);
}

esp_err_t jettyd_telemetry_publish_status(const char *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    static char status_buf[128];
    snprintf(status_buf, sizeof(status_buf),
             "{\"status\":\"%s\",\"ts\":%lld}",
             status, (long long)time(NULL));

    char topic[JETTYD_MQTT_MAX_TOPIC];
    esp_err_t err = jettyd_mqtt_build_topic(topic, sizeof(topic), "status");
    if (err != ESP_OK) {
        return err;
    }

    return jettyd_mqtt_publish(topic, status_buf, 1, false);
}
