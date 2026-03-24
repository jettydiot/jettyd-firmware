# jettyd-firmware

**ESP32 device SDK for [jettyd](https://jettyd.com)** — the IoT middleware layer for AI agents.

Flash this firmware on any ESP32. Your AI agent (OpenClaw, LangChain, custom) can then read sensors, send commands, and run automations in seconds — no custom protocol, no glue code.

---

## What's included

| Path | Description |
|------|-------------|
| `core/` | Core jettyd runtime: WiFi, MQTT, provisioning, telemetry, device shadow, OTA, JettyScript VM |
| `drivers/` | Sensor & actuator drivers: BME280, DHT22, DS18B20, HC-SR04, INA219, PWM output, relay, soil moisture |
| `devices/` | Example device configurations: env-monitor-v1, relay-controller-v1, soil-moisture-v1 |
| `test/` | Unit tests for drivers, telemetry, and VM |
| `tools/` | Safety review tooling (firmware static analysis) |

## Quick start

```bash
# Install ESP-IDF 5.x
# Then clone the template project:
git clone https://github.com/jettydiot/jettyd-firmware-template my-device
cd my-device

# Configure
idf.py menuconfig  # Set WiFi SSID/pass and jettyd fleet token

# Build and flash
idf.py build flash monitor
```

Your device self-registers on first boot. From your AI agent:

```python
# OpenClaw / any HTTP client
GET https://api.jettyd.com/v1/devices/{device_id}/shadow
POST https://api.jettyd.com/v1/devices/{device_id}/command
```

## Supported hardware

| Chip | Status |
|------|--------|
| ESP32-S3 | ✅ Primary target |
| ESP32-C3 | ✅ Supported |
| ESP32-C6 | ✅ Supported |
| ESP32 (classic) | ⚠️ No BLE provisioning |
| RPi Pico W | 🔜 Planned |

## Architecture

```
[Your ESP32]
    │  jettyd-firmware (this repo)
    │  ├─ WiFi + MQTT client
    │  ├─ Fleet provisioning (one-time, self-registers)
    │  ├─ Telemetry publisher
    │  ├─ Device shadow (desired ↔ reported state)
    │  ├─ OTA firmware updates
    │  └─ JettyScript VM (rules engine, runs on-device)
    │
    ▼ MQTT / TLS
[jettyd platform]  ←→  [Your AI agent]
  api.jettyd.com         OpenClaw skill
                         REST / WebSocket
```

## JettyScript

Rules that run on the device itself — no cloud round-trip required:

```json
{
  "rules": [{
    "id": "temp-alert",
    "when": { "type": "threshold", "sensor": "temperature", "op": ">", "value": 30 },
    "then": [{ "action": "publish_alert", "params": { "message": "Too hot!", "severity": "warn" }}]
  }]
}
```

Push config via MQTT → device validates, applies, and ACKs — all without reflashing.

## Contributing

Pull requests are welcome. For major changes, open an issue first.

Please follow the coding conventions in the existing drivers and ensure the safety review passes (`node tools/safety-review.js <file.c>`).

See [CONTRIBUTING.md](CONTRIBUTING.md).

## Licence

[MIT](LICENSE) — free to use, fork, and embed in your products.

---

Built in Johannesburg 🦦 · [jettyd.com](https://jettyd.com) · [docs](https://docs.jettyd.com) · [platform repo is private]
