# jettyd firmware SDK

Open-source C SDK for ESP-IDF that connects ESP32 devices to [jettyd](https://jettyd.com).

> **Getting started?** Use the [firmware template](https://github.com/jettydiot/jettyd-firmware-template) and follow the **[QuickStart guide →](https://github.com/jettydiot/jettyd-firmware-template/blob/main/QUICKSTART.md)** (also rendered at [docs.jettyd.com/quickstart](https://docs.jettyd.com/quickstart) and shown in-app at [app.jettyd.com](https://app.jettyd.com)).
>
> `QUICKSTART.md` in `jettyd-firmware-template` is the **single source of truth** — both the docs site and the dashboard render from it.

## What this SDK provides

- **Provisioning** — auto-register devices with the jettyd platform via MQTT
- **Telemetry** — publish sensor readings on configurable heartbeat intervals
- **Device Shadow** — reported/desired state sync with the platform
- **Commands** — receive and execute commands from the dashboard or AI agents, including runtime WiFi credential updates with rollback (see [docs/wifi-reconfiguration.md](docs/wifi-reconfiguration.md)) and LED matrix display via [display.set](docs/display-driver.md)
- **OTA** — over-the-air firmware updates
- **JettyScript VM** — local rule engine (if/then logic without cloud round-trips)
- **Driver framework** — pluggable sensor/actuator drivers with a standard interface
- **MCP tool dispatch** — AI agents invoke driver tools directly over MQTT (see [docs/mcp-tools.md](docs/mcp-tools.md))

## Drivers included

| Driver | Type | Metrics / Actions | MCP tools |
|--------|------|-------------------|-----------|
| `dht22` | Sensor | temperature, humidity | `dht22_read` |
| `bme280` | Sensor | temperature, humidity, pressure | `bme280_read` |
| `ds18b20` | Sensor | temperature | `ds18b20_read_temperature` |
| `soil_moisture` | Sensor | moisture (0–100%) | `soil_read_moisture` |
| `hcsr04` | Sensor | distance_cm | `hcsr04_measure_distance` |
| `ina219` | Sensor | voltage, current, power | `ina219_read_power` |
| `led` | Actuator | on, off, blink | `led_turn_on`, `led_turn_off`, `led_blink` |
| `relay` | Actuator | on, off | `relay_turn_on`, `relay_turn_off` |
| `button` | Input | press, long_press, double_press | `button_get_state` |
| `pwm_output` | Actuator | duty cycle (0–100%), frequency | `pwm_set_duty`, `pwm_set_freq` |
| `rgb_led_matrix` | Actuator | pattern display | `matrix_show`, `matrix_clear` |
| `servo` | Actuator | angle (0–180°), hold_ms | `servo_rotate` |
| `camera` | Sensor | upload_errors | `camera_capture` |
| `display` | Actuator | `display.set` (number/string → MAX7219 32×8) | `display_set` |

## System metrics

Built-in metrics available in telemetry heartbeats:

| Metric | Description |
|--------|-------------|
| `system.rssi` | WiFi signal strength (dBm) |
| `system.chip_temp` | Internal die temperature (°C) |
| `system.connected` | MQTT connection status (1/0) |
| `system.uptime` | Seconds since boot |
| `system.heap_free` | Free heap memory (bytes) |

## JettyScript

Declarative rules that run on the device — no cloud needed:

```json
{
  "rules": [
    {
      "id": "status-online",
      "condition": { "sensor": "system.connected", "op": "==", "value": 1 },
      "actions": [{ "action": "blink", "target": "status", "interval_ms": 5000 }]
    },
    {
      "id": "status-offline",
      "condition": { "sensor": "system.connected", "op": "==", "value": 0 },
      "actions": [{ "action": "blink", "target": "status", "interval_ms": 1000 }]
    }
  ]
}
```

### Available actions

| Action | Description | Params |
|--------|-------------|--------|
| `switch_on` | Turn driver on | `target`, `duration_sec` (optional) |
| `switch_off` | Turn driver off | `target` |
| `blink` | Blink driver at interval | `target`, `interval_ms` |
| `set_value` | Set driver value | `target`, `value` |
| `report` | Publish telemetry now | `metrics` (optional, default: all) |
| `alert` | Send alert to platform | `message`, `severity` |
| `sleep` | Enter deep sleep | `seconds` (max 60) |
| `set_heartbeat` | Change heartbeat interval | `interval_sec` (10–3600) |

## Supported hardware

- **ESP32-S3** ✅
- **ESP32-C3** ✅
- **ESP32-C6** ✅

Original ESP32 is not supported.

## Changing WiFi networks

Two mechanisms are available for updating a device's WiFi credentials after
first flash:

- **`wifi.set` command** — send a command from the dashboard or an agent while
  the device is online. The device reconnects to the new network with automatic
  rollback if it cannot reach the new SSID within the timeout.
- **SoftAP fallback portal** — when the device cannot connect at all (stale
  credentials, new router), power-cycle (or wait for `CONFIG_JETTYD_WIFI_PORTAL_FAIL_THRESHOLD`
  failures during the `CONFIG_JETTYD_WIFI_PORTAL_BOOT_WINDOW_S` boot window), connect
  to the `jettyd-<id>` open AP from a phone or laptop, and open
  `http://192.168.4.1` to enter new credentials.

See **[docs/wifi-reconfiguration.md](docs/wifi-reconfiguration.md)** for full
details on both mechanisms, including Kconfig options and the arming/security
model for the portal.

## Documentation

- **[QuickStart](https://docs.jettyd.com/quickstart)** — 5 minutes to first device
- **[API Reference](https://docs.jettyd.com/api)** — platform REST API
- **[Template](https://github.com/jettydiot/jettyd-firmware-template)** — starter project

## License

MIT — see [LICENSE](LICENSE).
