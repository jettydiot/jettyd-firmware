# MCP Tool Dispatch — Device-Side Protocol

FLU-137 adds Model Context Protocol (MCP) tool dispatch to the jettyd firmware SDK. AI agents can discover and invoke driver capabilities directly over MQTT without custom command wrappers.

## MQTT topics

All topics follow the standard jettyd namespace: `jettyd/{tenant_id}/{device_key}/`.

| Topic | Direction | QoS | Retain | Purpose |
|-------|-----------|-----|--------|---------|
| `…/tools/list` | device → platform | 1 | yes | Tool manifest, published on every connect |
| `…/mcp/call` | platform → device | 1 | no | Invoke a tool |
| `…/mcp/result` | device → platform | 1 | no | Tool result or error |

## tools/list payload

Published retained on every MQTT connect. JSON object:

```json
{
  "tools": [
    {
      "name": "relay_turn_on",
      "description": "Turn relay on (optionally for duration_s seconds)",
      "driver": "valve",
      "inputSchema": {
        "type": "object",
        "properties": {
          "duration_s": { "type": "number" }
        }
      }
    },
    {
      "name": "relay_turn_off",
      "description": "Turn relay off",
      "driver": "valve",
      "inputSchema": { "type": "object", "properties": {} }
    }
  ]
}
```

Fields:
- `name` — unique tool identifier, matches the `tool` field in `mcp/call`
- `description` — human-readable description for the LLM
- `driver` — instance name of the driver that owns this tool
- `inputSchema` — JSON Schema for the `params` object in `mcp/call`

## mcp/call payload

```json
{
  "id": "call_abc123",
  "tool": "relay_turn_on",
  "params": { "duration_s": 30 }
}
```

Fields:
- `id` — caller-chosen correlation ID, echoed in the result
- `tool` — tool name from `tools/list`
- `params` — optional object matching the tool's `inputSchema`

## mcp/result payload

Success:
```json
{ "id": "call_abc123", "status": "ok", "result": { "state": true } }
```

Error:
```json
{ "id": "call_abc123", "status": "error", "error": "unknown tool" }
```

## SDK API

### Registering tools in a driver

Add `mcp_tools` and `mcp_tool_count` to the driver struct during `*_register()`:

```c
#include "jettyd_driver.h"

static esp_err_t my_mcp_action(const char *params_json, char *out, size_t out_len)
{
    /* Parse params_json with strstr() — no heap allocation */
    int value = 0;
    if (params_json) {
        const char *p = strstr(params_json, "\"value\":");
        if (p) value = atoi(p + 8);
    }
    esp_err_t err = my_driver_do_thing(value);
    if (err == ESP_OK) snprintf(out, out_len, "{\"done\":true}");
    return err;
}

void my_driver_register(const char *instance, const void *config)
{
    /* ... capability registration ... */

    static const char *schema =
        "{\"type\":\"object\",\"properties\":{\"value\":{\"type\":\"number\"}}}";

    strlcpy(s_driver.mcp_tools[0].name, "my_action", sizeof(s_driver.mcp_tools[0].name));
    s_driver.mcp_tools[0].description = "Do the thing with value";
    s_driver.mcp_tools[0].input_schema_json = schema;
    s_driver.mcp_tools[0].handler = my_mcp_action;
    s_driver.mcp_tool_count = 1;

    JETTYD_REGISTER_DRIVER(&s_driver);
}
```

### Public API (`jettyd_mcp.h`)

```c
/* Called automatically by jettyd_start() — do not call directly */
esp_err_t jettyd_mcp_init(void);

/* Called automatically on MQTT connect — do not call directly */
esp_err_t jettyd_publish_mcp_tools(void);

/* Serialize all registered tools to buf as JSON.
 * Returns ESP_ERR_NO_MEM if buf is too small (4096 bytes recommended). */
esp_err_t jettyd_mcp_serialize_tools_list(char *buf, size_t buf_len);

/* Dispatch an incoming mcp/call payload to the matching tool handler. */
esp_err_t jettyd_mcp_handle_call(const char *payload, int payload_len);
```

## Included tools (all 11 drivers)

| Driver | Tool name | Description | Input params |
|--------|-----------|-------------|--------------|
| `relay` | `relay_turn_on` | Turn relay on | `duration_s` (optional, number) |
| `relay` | `relay_turn_off` | Turn relay off | — |
| `led` | `led_turn_on` | Turn LED on | — |
| `led` | `led_turn_off` | Turn LED off | — |
| `led` | `led_blink` | Blink LED | `interval_ms` (number), `count` (number) |
| `button` | `button_get_state` | Read button press state | — |
| `bme280` | `bme280_read` | Read temperature, humidity, pressure | — |
| `dht22` | `dht22_read` | Read temperature and humidity | — |
| `ds18b20` | `ds18b20_read_temperature` | Read temperature | — |
| `hcsr04` | `hcsr04_measure_distance` | Measure ultrasonic distance in cm | — |
| `ina219` | `ina219_read_power` | Read voltage, current, and power | — |
| `pwm_output` | `pwm_set_duty` | Set PWM duty cycle (0–100%) | `duty` (number) |
| `pwm_output` | `pwm_set_freq` | Set PWM frequency in Hz | `freq` (number) |
| `rgb_led_matrix` | `matrix_show` | Display pattern on matrix | `pattern` (string), `color` (string) |
| `rgb_led_matrix` | `matrix_clear` | Clear matrix display | — |
| `soil_moisture` | `soil_read_moisture` | Read soil moisture percentage | — |

## Implementation constraints

- No heap allocation. Tool handlers use `snprintf` into caller-provided buffers.
- `description` and `input_schema_json` are `const char *` pointers to flash-resident strings.
- Maximum tools per driver: `JETTYD_MAX_MCP_TOOLS` (8).
- Maximum total drivers: `JETTYD_MAX_DRIVERS` (16).
- `tools/list` buffer: 4096 bytes static (stack-safe, reused across reconnects).
- `mcp/call` payload maximum: 512 bytes.
- `mcp/result` payload maximum: 384 bytes.
