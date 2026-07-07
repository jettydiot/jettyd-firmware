/**
 * @file mcp.c
 * @brief Device-side MCP tool dispatch (FLU-137).
 *
 * Implements:
 *   - tools/list JSON serializer
 *   - Retained MQTT publish of tools/list on connect
 *   - mcp/call subscription and dispatch to driver tool handlers
 *   - mcp/result publish
 */

#include "jettyd_mcp.h"
#include "jettyd_driver.h"
#include "jettyd_mqtt.h"
#include "jettyd_provision.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

static const char *TAG = "jettyd_mcp";

#define MCP_CALL_MAX_LEN    512
#define MCP_ID_LEN           64
#define MCP_TOOL_NAME_LEN    64
#define MCP_RESULT_MAX_LEN  256
#define MCP_TOOLS_BUF_LEN  4096

/* ── JSON helpers ─────────────────────────────────────────────────────────── */

/* Extract a string-valued field: "key":"value" → value in out */
static bool json_get_str(const char *json, const char *key, char *out, size_t out_len)
{
    char search[80];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char *p = strstr(json, search);
    if (!p) return false;
    p += strlen(search);
    size_t i = 0;
    while (*p && *p != '"' && i < out_len - 1) out[i++] = *p++;
    out[i] = '\0';
    return (*p == '"');
}

/* Find the object value of a key: "key":{ → returns pointer to '{' */
static const char *json_find_obj(const char *json, const char *key)
{
    char search[80];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return NULL;
    p += strlen(search);
    while (*p == ' ') p++;
    return (*p == '{') ? p : NULL;
}

/* ── Result publishing ─────────────────────────────────────────────────────── */

static esp_err_t publish_mcp_result(const char *call_id, bool ok, const char *body)
{
    char topic[JETTYD_MQTT_MAX_TOPIC];
    if (jettyd_mqtt_build_topic(topic, sizeof(topic), "mcp/result") != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    char out[MCP_RESULT_MAX_LEN + 128];
    if (ok) {
        snprintf(out, sizeof(out), "{\"id\":\"%s\",\"status\":\"ok\",\"result\":%s}",
                 call_id, (body && body[0]) ? body : "{}");
    } else {
        snprintf(out, sizeof(out), "{\"id\":\"%s\",\"status\":\"error\",\"error\":\"%s\"}",
                 call_id, (body && body[0]) ? body : "error");
    }
    return jettyd_mqtt_publish(topic, out, 1, false);
}

/* ── Public API ─────────────────────────────────────────────────────────────── */

esp_err_t jettyd_mcp_serialize_tools_list(char *buf, size_t buf_len)
{
    if (!buf || buf_len < 12) return ESP_ERR_INVALID_ARG;

    int pos = 0;
    int rem = (int)buf_len;
    int w;

    w = snprintf(buf, (size_t)rem, "{\"tools\":[");
    if (w < 0 || w >= rem) return ESP_ERR_NO_MEM;
    pos += w; rem -= w;

    bool first = true;
    uint8_t count = jettyd_driver_count();
    for (uint8_t i = 0; i < count; i++) {
        const jettyd_driver_t *drv = jettyd_driver_get(i);
        if (!drv || drv->mcp_tool_count == 0) continue;

        for (uint8_t j = 0; j < drv->mcp_tool_count; j++) {
            const jettyd_mcp_tool_t *t = &drv->mcp_tools[j];
            if (t->name[0] == '\0') continue;

            const char *desc = t->description ? t->description : "";
            const char *schema = t->input_schema_json
                ? t->input_schema_json
                : "{\"type\":\"object\",\"properties\":{}}";

            w = snprintf(buf + pos, (size_t)rem,
                         "%s{\"name\":\"%s\",\"description\":\"%s\","
                         "\"driver\":\"%s\",\"inputSchema\":%s}",
                         first ? "" : ",",
                         t->name, desc, drv->instance, schema);
            if (w < 0 || w >= rem) return ESP_ERR_NO_MEM;
            pos += w; rem -= w;
            first = false;
        }
    }

    w = snprintf(buf + pos, (size_t)rem, "]}");
    if (w < 0 || w >= rem) return ESP_ERR_NO_MEM;

    return ESP_OK;
}

esp_err_t jettyd_publish_mcp_tools(void)
{
    const jettyd_provision_state_t *prov = jettyd_provision_get_state();
    if (!prov || !prov->provisioned) {
        ESP_LOGD(TAG, "Not provisioned — skipping MCP tools publish");
        return ESP_OK;
    }
    if (!jettyd_mqtt_is_connected()) {
        ESP_LOGD(TAG, "MQTT not connected — skipping MCP tools publish");
        return ESP_OK;
    }

    char topic[JETTYD_MQTT_MAX_TOPIC];
    if (jettyd_mqtt_build_topic(topic, sizeof(topic), "tools/list") != ESP_OK) {
        return ESP_FAIL;
    }

    static char s_tools_buf[MCP_TOOLS_BUF_LEN];
    esp_err_t err = jettyd_mcp_serialize_tools_list(s_tools_buf, sizeof(s_tools_buf));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to serialize tools list (err=%d)", err);
        return err;
    }

    ESP_LOGI(TAG, "Publishing MCP tools list (%d drivers)", jettyd_driver_count());
    return jettyd_mqtt_publish(topic, s_tools_buf, 1, true);
}

esp_err_t jettyd_mcp_handle_call(const char *payload, int payload_len)
{
    if (!payload || payload_len == 0) return ESP_ERR_INVALID_ARG;

    char buf[MCP_CALL_MAX_LEN];
    int copy_len = payload_len < (int)(sizeof(buf) - 1)
                   ? payload_len : (int)(sizeof(buf) - 1);
    memcpy(buf, payload, (size_t)copy_len);
    buf[copy_len] = '\0';

    char call_id[MCP_ID_LEN]         = {0};
    char tool_name[MCP_TOOL_NAME_LEN] = {0};

    if (!json_get_str(buf, "id", call_id, sizeof(call_id))) {
        strncpy(call_id, "unknown", sizeof(call_id) - 1);
    }
    if (!json_get_str(buf, "tool", tool_name, sizeof(tool_name))) {
        ESP_LOGW(TAG, "mcp/call missing 'tool' field");
        return publish_mcp_result(call_id, false, "missing tool field");
    }

    ESP_LOGI(TAG, "MCP call: id=%s tool=%s", call_id, tool_name);

    const char *params = json_find_obj(buf, "params");

    uint8_t drv_count = jettyd_driver_count();
    for (uint8_t i = 0; i < drv_count; i++) {
        const jettyd_driver_t *drv = jettyd_driver_get(i);
        if (!drv) continue;
        for (uint8_t j = 0; j < drv->mcp_tool_count; j++) {
            if (strcmp(drv->mcp_tools[j].name, tool_name) == 0) {
                if (!drv->mcp_tools[j].handler) {
                    return publish_mcp_result(call_id, false, "tool has no handler");
                }
                char result[MCP_RESULT_MAX_LEN] = {0};
                esp_err_t err = drv->mcp_tools[j].handler(params, result, sizeof(result));
                ESP_LOGI(TAG, "MCP tool %s: %s", tool_name, err == ESP_OK ? "ok" : "error");
                return publish_mcp_result(call_id, err == ESP_OK,
                                          err == ESP_OK ? result : "handler failed");
            }
        }
    }

    ESP_LOGW(TAG, "MCP unknown tool: %s", tool_name);
    return publish_mcp_result(call_id, false, "unknown tool");
}

static void mcp_call_cb(const char *topic, const char *data, int data_len)
{
    (void)topic;
    jettyd_mcp_handle_call(data, data_len);
}

esp_err_t jettyd_mcp_init(void)
{
    char topic[JETTYD_MQTT_MAX_TOPIC];
    if (jettyd_mqtt_build_topic(topic, sizeof(topic), "mcp/call") != ESP_OK) {
        ESP_LOGE(TAG, "Failed to build mcp/call topic");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "Subscribing to MCP call topic: %s", topic);
    return jettyd_mqtt_subscribe(topic, 1, mcp_call_cb);
}
