# jettyd firmware SDK

Open-source C SDK for ESP-IDF that connects ESP32 devices to [jettyd](https://jettyd.com).

> **Getting started?** Use the [firmware template](https://github.com/jettydiot/jettyd-firmware-template) and follow the **[QuickStart guide →](https://github.com/jettydiot/jettyd-firmware-template/blob/main/QUICKSTART.md)** (also rendered at [docs.jettyd.com/quickstart](https://docs.jettyd.com/quickstart) and shown in-app at [app.jettyd.com](https://app.jettyd.com)).
>
> `QUICKSTART.md` in `jettyd-firmware-template` is the **single source of truth** — both the docs site and the dashboard render from it.

## What this SDK provides

- **Provisioning** — auto-register devices with the jettyd platform via MQTT
- **Telemetry** — publish sensor readings on configurable heartbeat intervals
- **Device Shadow** — reported/desired state sync with the platform
- **Commands** — receive and execute commands from the dashboard or AI agents
- **OTA** — over-the-air firmware updates
- **JettyScript VM** — local rule engine (if/then logic without cloud round-trips)
- **Driver framework** — pluggable sensor/actuator drivers with a standard interface

## Drivers included

| Driver | Type | Metrics / Actions |
|--------|------|-------------------|
| `dht22` | Sensor | temperature, humidity |
| `bme280` | Sensor | temperature, humidity, pressure |
| `ds18b20` | Sensor | temperature |
| `soil_moisture` | Sensor | moisture (0–100%) |
| `hcsr04` | Sensor | distance_cm |
| `ina219` | Sensor | voltage, current, power |
| `led` | Actuator | on, off, blink |
| `relay` | Actuator | on, off |
| `button` | Input | press, long_press, double_press |
| `pwm_output` | Actuator | duty cycle (0–100%) |

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

## Documentation

- **[QuickStart](https://docs.jettyd.com/quickstart)** — 5 minutes to first device
- **[API Reference](https://docs.jettyd.com/api)** — platform REST API
- **[Template](https://github.com/jettydiot/jettyd-firmware-template)** — starter project

## License

MIT — see [LICENSE](LICENSE).
