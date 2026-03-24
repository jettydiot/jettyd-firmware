/**
 * @file jettyd_mqtt.h
 * @brief MQTT client interface for the Jettyd firmware SDK.
 *
 * Handles connection to the MQTT broker, topic subscription, publishing,
 * and offline message buffering.
 */

#ifndef JETTYD_MQTT_H
#define JETTYD_MQTT_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/** Maximum offline message buffer entries */
#define JETTYD_MQTT_MAX_BUFFER  8

/** Maximum topic length */
#define JETTYD_MQTT_MAX_TOPIC   128

/** Maximum payload length */
#define JETTYD_MQTT_MAX_PAYLOAD 512

/**
 * @brief Callback for received MQTT messages.
 *
 * @param topic   Full topic string.
 * @param data    Message payload (not null-terminated).
 * @param data_len Payload length.
 */
typedef void (*jettyd_mqtt_msg_cb_t)(const char *topic, const char *data, int data_len);

/**
 * @brief MQTT configuration.
 */
typedef struct {
    const char *broker_uri;         /**< e.g., "mqtts://mqtt.jettyd.com:8883" */
    const char *username;           /**< Device name or fleet token ID */
    const char *password;           /**< Device key (dk_...) or fleet token (ft_...) */
    const char *client_id;          /**< MQTT client ID */
    uint32_t keepalive_sec;         /**< Keepalive interval */
    uint8_t qos;                    /**< Default QoS */
    bool buffer_on_disconnect;      /**< Buffer messages when disconnected */
    uint16_t max_buffer_size;       /**< Max offline buffer entries */
} jettyd_mqtt_config_t;

/**
 * @brief Initialize the MQTT client.
 */
esp_err_t jettyd_mqtt_init(const jettyd_mqtt_config_t *config);

/**
 * @brief Connect to the MQTT broker.
 */
esp_err_t jettyd_mqtt_connect(void);

/**
 * @brief Disconnect from the MQTT broker.
 */
esp_err_t jettyd_mqtt_disconnect(void);

/**
 * @brief Check if connected to the MQTT broker.
 */
bool jettyd_mqtt_is_connected(void);

/**
 * @brief Subscribe to a topic with a message callback.
 *
 * @param topic  Topic filter to subscribe to.
 * @param qos    QoS level (0, 1, or 2).
 * @param cb     Callback invoked for each message on this topic.
 */
esp_err_t jettyd_mqtt_subscribe(const char *topic, uint8_t qos, jettyd_mqtt_msg_cb_t cb);

/**
 * @brief Publish a message to a topic.
 *
 * If disconnected and buffering is enabled, the message is queued.
 *
 * @param topic   Topic to publish to.
 * @param data    Payload (null-terminated string).
 * @param qos     QoS level.
 * @param retain  Retain flag.
 */
esp_err_t jettyd_mqtt_publish(const char *topic, const char *data, uint8_t qos, bool retain);

/**
 * @brief Reconfigure MQTT credentials (used after provisioning).
 *
 * Disconnects, updates credentials, and reconnects.
 */
esp_err_t jettyd_mqtt_reconfigure(const char *username, const char *password);

/**
 * @brief Build a full topic string: jettyd/{tenant_id}/{device_key}/{suffix}
 *
 * @param buf      Output buffer.
 * @param buf_len  Buffer size.
 * @param suffix   Topic suffix (e.g., "telemetry", "command/response").
 */
esp_err_t jettyd_mqtt_build_topic(char *buf, size_t buf_len, const char *suffix);

/**
 * @brief Flush any buffered offline messages.
 *
 * Called automatically on reconnect.
 */
esp_err_t jettyd_mqtt_flush_buffer(void);

#endif /* JETTYD_MQTT_H */
