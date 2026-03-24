/**
 * @file mqtt.c
 * @brief MQTT client implementation with offline buffering.
 */

#include "jettyd_mqtt.h"
#include "jettyd_provision.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "jettyd_mqtt";

/* Subscription registry */
#define MAX_SUBSCRIPTIONS 16

typedef struct {
    char topic[JETTYD_MQTT_MAX_TOPIC];
    jettyd_mqtt_msg_cb_t callback;
} mqtt_subscription_t;

/* Offline message buffer */
typedef struct {
    char topic[JETTYD_MQTT_MAX_TOPIC];
    char payload[JETTYD_MQTT_MAX_PAYLOAD];
    uint8_t qos;
    bool retain;
    bool used;
} mqtt_buffered_msg_t;

static esp_mqtt_client_handle_t s_client = NULL;
static volatile bool s_connected = false;
static jettyd_mqtt_config_t s_config;
static mqtt_subscription_t s_subs[MAX_SUBSCRIPTIONS];
static uint8_t s_sub_count = 0;
static mqtt_buffered_msg_t s_buffer[JETTYD_MQTT_MAX_BUFFER];
static SemaphoreHandle_t s_mutex = NULL;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");
        s_connected = true;

        /* Re-subscribe to all registered topics */
        for (uint8_t i = 0; i < s_sub_count; i++) {
            esp_mqtt_client_subscribe(s_client, s_subs[i].topic, s_config.qos);
        }

        /* Flush buffered messages */
        jettyd_mqtt_flush_buffer();
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        s_connected = false;
        break;

    case MQTT_EVENT_DATA: {
        /* Find matching subscription and invoke callback */
        char topic_buf[JETTYD_MQTT_MAX_TOPIC];
        int topic_len = event->topic_len < (int)(sizeof(topic_buf) - 1)
                        ? event->topic_len : (int)(sizeof(topic_buf) - 1);
        memcpy(topic_buf, event->topic, topic_len);
        topic_buf[topic_len] = '\0';

        for (uint8_t i = 0; i < s_sub_count; i++) {
            /* Simple prefix match for wildcard subscriptions */
            if (strcmp(s_subs[i].topic, topic_buf) == 0 ||
                (s_subs[i].topic[strlen(s_subs[i].topic) - 1] == '#' &&
                 strncmp(s_subs[i].topic, topic_buf, strlen(s_subs[i].topic) - 1) == 0)) {
                if (s_subs[i].callback) {
                    s_subs[i].callback(topic_buf, event->data, event->data_len);
                }
            }
        }
        break;
    }

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error type: %d", event->error_handle->error_type);
        break;

    default:
        break;
    }
}

esp_err_t jettyd_mqtt_init(const jettyd_mqtt_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(&s_config, config, sizeof(jettyd_mqtt_config_t));
    s_sub_count = 0;
    memset(s_buffer, 0, sizeof(s_buffer));

    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = config->broker_uri,
        .credentials.username = config->username,
        .credentials.authentication.password = config->password,
        .credentials.client_id = config->client_id,
        .session.keepalive = config->keepalive_sec,
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "Failed to init MQTT client");
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);

    ESP_LOGI(TAG, "MQTT initialized (broker: %s)", config->broker_uri);
    return ESP_OK;
}

esp_err_t jettyd_mqtt_connect(void)
{
    if (s_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_mqtt_client_start(s_client);
}

esp_err_t jettyd_mqtt_disconnect(void)
{
    if (s_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_connected = false;
    return esp_mqtt_client_stop(s_client);
}

bool jettyd_mqtt_is_connected(void)
{
    return s_connected;
}

esp_err_t jettyd_mqtt_subscribe(const char *topic, uint8_t qos, jettyd_mqtt_msg_cb_t cb)
{
    if (topic == NULL || cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_sub_count >= MAX_SUBSCRIPTIONS) {
        ESP_LOGE(TAG, "Subscription table full");
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strncpy(s_subs[s_sub_count].topic, topic, JETTYD_MQTT_MAX_TOPIC - 1);
    s_subs[s_sub_count].callback = cb;
    s_sub_count++;
    xSemaphoreGive(s_mutex);

    if (s_connected && s_client) {
        esp_mqtt_client_subscribe(s_client, topic, qos);
    }

    ESP_LOGI(TAG, "Subscribed: %s", topic);
    return ESP_OK;
}

esp_err_t jettyd_mqtt_publish(const char *topic, const char *data, uint8_t qos, bool retain)
{
    if (topic == NULL || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_connected && s_client) {
        int msg_id = esp_mqtt_client_publish(s_client, topic, data, 0, qos, retain ? 1 : 0);
        if (msg_id < 0) {
            ESP_LOGE(TAG, "Publish failed for topic: %s", topic);
            return ESP_FAIL;
        }
        return ESP_OK;
    }

    /* Buffer the message if disconnected and buffering is enabled */
    if (s_config.buffer_on_disconnect) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        for (uint16_t i = 0; i < s_config.max_buffer_size && i < JETTYD_MQTT_MAX_BUFFER; i++) {
            if (!s_buffer[i].used) {
                strncpy(s_buffer[i].topic, topic, JETTYD_MQTT_MAX_TOPIC - 1);
                strncpy(s_buffer[i].payload, data, JETTYD_MQTT_MAX_PAYLOAD - 1);
                s_buffer[i].qos = qos;
                s_buffer[i].retain = retain;
                s_buffer[i].used = true;
                xSemaphoreGive(s_mutex);
                ESP_LOGD(TAG, "Buffered message for topic: %s", topic);
                return ESP_OK;
            }
        }
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "Offline buffer full, dropping message for: %s", topic);
        return ESP_ERR_NO_MEM;
    }

    return ESP_ERR_INVALID_STATE;
}

esp_err_t jettyd_mqtt_reconfigure(const char *username, const char *password)
{
    if (s_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Stop current connection */
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_connected = false;

    /* Update config */
    s_config.username = username;
    s_config.password = password;

    /* Re-init with new credentials */
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = s_config.broker_uri,
        .credentials.username = username,
        .credentials.authentication.password = password,
        .credentials.client_id = s_config.client_id,
        .session.keepalive = s_config.keepalive_sec,
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_client == NULL) {
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    return esp_mqtt_client_start(s_client);
}

esp_err_t jettyd_mqtt_build_topic(char *buf, size_t buf_len, const char *suffix)
{
    const jettyd_provision_state_t *prov = jettyd_provision_get_state();
    if (prov == NULL || !prov->provisioned) {
        return ESP_ERR_INVALID_STATE;
    }

    int written = snprintf(buf, buf_len, "jettyd/%s/%s/%s",
                           prov->tenant_id, prov->device_key, suffix);
    if (written < 0 || (size_t)written >= buf_len) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t jettyd_mqtt_flush_buffer(void)
{
    if (!s_connected || s_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int flushed = 0;
    for (uint16_t i = 0; i < JETTYD_MQTT_MAX_BUFFER; i++) {
        if (s_buffer[i].used) {
            esp_mqtt_client_publish(s_client, s_buffer[i].topic,
                                    s_buffer[i].payload, 0,
                                    s_buffer[i].qos,
                                    s_buffer[i].retain ? 1 : 0);
            s_buffer[i].used = false;
            flushed++;
        }
    }
    xSemaphoreGive(s_mutex);

    if (flushed > 0) {
        ESP_LOGI(TAG, "Flushed %d buffered messages", flushed);
    }
    return ESP_OK;
}
