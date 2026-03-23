/**
 * @file shadow.c
 * @brief Device shadow: local state cache published to platform.
 */

#include "jettyd_shadow.h"
#include "jettyd_nvs.h"
#include "jettyd_mqtt.h"
#include "jettyd_vm.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "jettyd_shadow";

#define MAX_SHADOW_ENTRIES 32

typedef struct {
    char key[48];           /* e.g., "soil.moisture" or "system.uptime" */
    jettyd_value_t value;
    bool used;
} shadow_entry_t;

static shadow_entry_t s_reported[MAX_SHADOW_ENTRIES];
static char s_desired_json[JETTYD_SHADOW_MAX_SIZE];
static bool s_has_desired = false;
static SemaphoreHandle_t s_mutex = NULL;

static shadow_entry_t *find_or_create_entry(const char *key)
{
    /* Find existing */
    for (int i = 0; i < MAX_SHADOW_ENTRIES; i++) {
        if (s_reported[i].used && strcmp(s_reported[i].key, key) == 0) {
            return &s_reported[i];
        }
    }
    /* Create new */
    for (int i = 0; i < MAX_SHADOW_ENTRIES; i++) {
        if (!s_reported[i].used) {
            strncpy(s_reported[i].key, key, sizeof(s_reported[i].key) - 1);
            s_reported[i].used = true;
            return &s_reported[i];
        }
    }
    return NULL;
}

esp_err_t jettyd_shadow_init(void)
{
    memset(s_reported, 0, sizeof(s_reported));
    memset(s_desired_json, 0, sizeof(s_desired_json));
    s_has_desired = false;

    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
    }

    /* Try to load last shadow from NVS */
    size_t len = JETTYD_SHADOW_MAX_SIZE;
    char buf[JETTYD_SHADOW_MAX_SIZE];
    esp_err_t err = jettyd_nvs_read_blob(JETTYD_NVS_NS_SHADOW, "shadow", buf, &len);
    if (err == ESP_OK && len > 0) {
        ESP_LOGI(TAG, "Loaded shadow from NVS (%d bytes)", (int)len);
    }

    ESP_LOGI(TAG, "Shadow initialized");
    return ESP_OK;
}

esp_err_t jettyd_shadow_update(const char *dotted_name, jettyd_value_t value)
{
    if (dotted_name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    shadow_entry_t *entry = find_or_create_entry(dotted_name);
    if (entry == NULL) {
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "Shadow full, cannot track: %s", dotted_name);
        return ESP_ERR_NO_MEM;
    }
    entry->value = value;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t jettyd_shadow_update_system(const char *key, jettyd_value_t value)
{
    char dotted[48];
    snprintf(dotted, sizeof(dotted), "system.%s", key);
    return jettyd_shadow_update(dotted, value);
}

esp_err_t jettyd_shadow_set_desired(const char *json, int len)
{
    if (json == NULL || len <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((size_t)len >= sizeof(s_desired_json)) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(s_desired_json, json, len);
    s_desired_json[len] = '\0';
    s_has_desired = true;
    xSemaphoreGive(s_mutex);

    return ESP_OK;
}

int jettyd_shadow_serialize(char *buf, size_t buf_len)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *reported = cJSON_CreateObject();
    cJSON *metadata = cJSON_CreateObject();

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    for (int i = 0; i < MAX_SHADOW_ENTRIES; i++) {
        if (!s_reported[i].used || !s_reported[i].value.valid) {
            continue;
        }
        switch (s_reported[i].value.type) {
        case JETTYD_VAL_FLOAT:
            cJSON_AddNumberToObject(reported, s_reported[i].key,
                                    s_reported[i].value.float_val);
            break;
        case JETTYD_VAL_INT:
            cJSON_AddNumberToObject(reported, s_reported[i].key,
                                    s_reported[i].value.int_val);
            break;
        case JETTYD_VAL_BOOL:
            cJSON_AddBoolToObject(reported, s_reported[i].key,
                                  s_reported[i].value.bool_val);
            break;
        case JETTYD_VAL_STRING:
            cJSON_AddStringToObject(reported, s_reported[i].key,
                                    s_reported[i].value.str_val);
            break;
        }
    }

    /* VM state */
    const jettyd_vm_state_t *vm = jettyd_vm_get_state();
    if (vm != NULL) {
        cJSON_AddNumberToObject(reported, "vm.rules_loaded", vm->rule_count);
        cJSON_AddNumberToObject(reported, "vm.heartbeats_loaded", vm->heartbeat_count);
    }

    xSemaphoreGive(s_mutex);

    cJSON_AddItemToObject(root, "reported", reported);

    /* Desired */
    if (s_has_desired) {
        cJSON *desired = cJSON_Parse(s_desired_json);
        cJSON_AddItemToObject(root, "desired", desired ? desired : cJSON_CreateNull());
    } else {
        cJSON_AddNullToObject(root, "desired");
    }

    /* Metadata */
    cJSON_AddNumberToObject(metadata, "last_report", (double)time(NULL));
    cJSON_AddItemToObject(root, "metadata", metadata);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str == NULL) {
        return -1;
    }

    int len = (int)strlen(json_str);
    if ((size_t)len >= buf_len) {
        cJSON_free(json_str);
        return -1;
    }

    memcpy(buf, json_str, len + 1);
    cJSON_free(json_str);
    return len;
}

esp_err_t jettyd_shadow_publish(void)
{
    char buf[JETTYD_SHADOW_MAX_SIZE];
    int len = jettyd_shadow_serialize(buf, sizeof(buf));
    if (len < 0) {
        ESP_LOGE(TAG, "Failed to serialize shadow");
        return ESP_FAIL;
    }

    char topic[JETTYD_MQTT_MAX_TOPIC];
    esp_err_t err = jettyd_mqtt_build_topic(topic, sizeof(topic), "shadow/report");
    if (err != ESP_OK) {
        return err;
    }

    return jettyd_mqtt_publish(topic, buf, 1, false);
}

esp_err_t jettyd_shadow_persist(void)
{
    char buf[JETTYD_SHADOW_MAX_SIZE];
    int len = jettyd_shadow_serialize(buf, sizeof(buf));
    if (len < 0) {
        return ESP_FAIL;
    }
    return jettyd_nvs_write_blob(JETTYD_NVS_NS_SHADOW, "shadow", buf, (size_t)len);
}

esp_err_t jettyd_shadow_reconcile(void)
{
    if (!s_has_desired) {
        return ESP_OK;
    }

    cJSON *desired = cJSON_Parse(s_desired_json);
    if (desired == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Iterate desired state and apply to drivers */
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, desired) {
        const char *key = item->string;
        if (key == NULL) continue;

        const jettyd_driver_t *drv = jettyd_driver_find_capability(key);
        if (drv == NULL) continue;

        /* Parse "instance.capability" */
        const char *dot = strchr(key, '.');
        if (dot == NULL) continue;
        const char *cap_name = dot + 1;

        /* Check capability type and apply */
        for (uint8_t i = 0; i < drv->capability_count; i++) {
            if (strcmp(drv->capabilities[i].name, cap_name) != 0) continue;

            if (drv->capabilities[i].type == JETTYD_CAP_SWITCHABLE) {
                if (cJSON_IsBool(item)) {
                    if (cJSON_IsTrue(item) && drv->switch_on) {
                        drv->switch_on(0);
                    } else if (drv->switch_off) {
                        drv->switch_off();
                    }
                }
            } else if (drv->capabilities[i].type == JETTYD_CAP_WRITABLE && drv->write) {
                jettyd_value_t val = {.type = JETTYD_VAL_FLOAT, .valid = true};
                if (cJSON_IsNumber(item)) {
                    val.float_val = (float)item->valuedouble;
                    drv->write(cap_name, val);
                }
            }
            break;
        }
    }

    cJSON_Delete(desired);
    s_has_desired = false;
    memset(s_desired_json, 0, sizeof(s_desired_json));

    /* Publish updated shadow */
    jettyd_shadow_publish();

    return ESP_OK;
}

void jettyd_shadow_desired_handler(const char *topic, const char *data, int data_len)
{
    ESP_LOGI(TAG, "Received desired state update");
    esp_err_t err = jettyd_shadow_set_desired(data, data_len);
    if (err == ESP_OK) {
        jettyd_shadow_reconcile();
    }
}
