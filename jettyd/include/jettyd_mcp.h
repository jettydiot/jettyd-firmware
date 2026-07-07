/**
 * @file jettyd_mcp.h
 * @brief Device-side MCP tool dispatch interface (FLU-137).
 *
 * Provides tools/list manifest publication and mcp/call dispatch.
 * Drivers register MCP tools in their jettyd_driver_t at boot.
 */

#ifndef JETTYD_MCP_H
#define JETTYD_MCP_H

#include "esp_err.h"
#include <stddef.h>

/**
 * @brief Subscribe to the mcp/call topic for incoming tool invocations.
 *
 * Call this from jettyd_start() after provisioning and topic subscription.
 */
esp_err_t jettyd_mcp_init(void);

/**
 * @brief Publish the tools/list manifest to the retained MQTT topic.
 *
 * Called on every MQTT connect/reconnect so the broker always holds
 * an up-to-date retained copy of the device's tool catalogue.
 */
esp_err_t jettyd_publish_mcp_tools(void);

/**
 * @brief Serialize all registered MCP tools to a JSON tools/list payload.
 *
 * Output format:
 *   {"tools":[{"name":"...","description":"...","driver":"...","inputSchema":{...}},...]}
 *
 * @param buf      Output buffer.
 * @param buf_len  Buffer size. Returns ESP_ERR_NO_MEM if too small.
 */
esp_err_t jettyd_mcp_serialize_tools_list(char *buf, size_t buf_len);

/**
 * @brief Dispatch an mcp/call payload to the appropriate tool handler.
 *
 * Exposed for unit testing. In production this is invoked by the MQTT
 * subscription callback.
 *
 * @param payload      Raw MQTT payload bytes (need not be null-terminated).
 * @param payload_len  Number of bytes in payload.
 */
esp_err_t jettyd_mcp_handle_call(const char *payload, int payload_len);

#endif /* JETTYD_MCP_H */
