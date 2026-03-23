# Jettyd Firmware SDK

ESP-IDF v5.5+ firmware SDK for connecting IoT devices to the Jettyd platform.

## Architecture

Three layers with clear boundaries:

1. **Peripheral drivers** (compile-time) — sensor reads, actuator controls
2. **Core runtime** (always compiled) — WiFi, MQTT, OTA, provisioning, shadow, telemetry
3. **JettyScript VM** (runtime) — declarative rules pushed over MQTT, no reflash needed

## Supported Hardware

| Target    | Description            |
|-----------|------------------------|
| ESP32-S3  | Primary, most features |
| ESP32-C3  | Low-cost, minimal RAM  |
| ESP32-C6  | WiFi 6, Thread/Zigbee  |

## Device Types

| Device              | Target  | Drivers                          |
|---------------------|---------|----------------------------------|
| soil-moisture-v1    | esp32s3 | soil_moisture, dht22, relay      |
| env-monitor-v1      | esp32c3 | dht22, ds18b20, soil_moisture    |
| relay-controller-v1 | esp32c6 | relay x4                         |

## Building

### Prerequisites

- ESP-IDF v5.5+ installed and sourced (`source $IDF_PATH/export.sh`)
- Python 3.8+ with `pyyaml` (`pip install pyyaml`)

### Build a device

```bash
cd apps/firmware
python tools/build.py --device soil-moisture-v1 --target esp32s3
```

This will:
1. Read `devices/soil-moisture-v1/device.yaml`
2. Generate `driver_init.c` and `driver_manifest.h` in the device's build dir
3. Invoke `idf.py build` with the correct component configuration

To generate files without building:
```bash
python tools/build.py --device soil-moisture-v1 --generate-only
```

### Build all device types

```bash
for device in soil-moisture-v1 env-monitor-v1 relay-controller-v1; do
    python tools/build.py --device $device
done
```

## Flashing + Provisioning

```bash
python tools/flash.py \
  --device soil-moisture-v1 \
  --port /dev/ttyUSB0 \
  --tenant-id tn_abc123 \
  --fleet-token ft_live_xxxxx
```

This flashes the firmware binary and writes provisioning credentials (WiFi SSID/password, fleet token, MQTT broker URI) to the NVS partition.

On first boot, the device:
1. Connects to WiFi
2. Connects to MQTT with the fleet token
3. Exchanges the fleet token for a permanent device key
4. Stores the device key in NVS and deletes the fleet token
5. Reconnects with device credentials

## Running Tests

Tests use mock drivers and run on the host (no hardware needed):

```bash
cd apps/firmware

# Build for Linux target
idf.py --target linux build

# Run unit tests
./build/test_vm           # JettyScript VM tests
./build/test_drivers      # Driver registry tests
./build/test_telemetry    # Message format tests
```

### Test coverage

- **test_vm.c** — Config loading, validation, edge detection, debounce, heartbeat timing, config replacement, template substitution, size/count limits
- **test_drivers.c** — Driver registration, lookup by instance, lookup by dotted name, duplicate rejection, registry limits
- **test_telemetry.c** — JSON format verification for telemetry, commands, shadow, alerts, provisioning, OTA

## P0 Drivers

| Driver         | Type     | Capabilities            |
|----------------|----------|-------------------------|
| relay          | actuator | switchable (on/off)     |
| soil_moisture  | sensor   | moisture (float %)      |
| dht22          | sensor   | temperature, humidity   |
| ds18b20        | sensor   | temperature             |

## JettyScript Rules

Rules are pushed to devices over MQTT on the `config` topic. Example:

```json
{
  "version": 1,
  "rules": [{
    "id": "r1",
    "name": "auto-irrigate",
    "enabled": true,
    "when": {
      "type": "threshold",
      "sensor": "soil.moisture",
      "op": "<",
      "value": 30,
      "debounce": 300
    },
    "then": [{
      "action": "switch_on",
      "target": "valve",
      "params": {"duration": 300}
    }]
  }],
  "heartbeats": [{
    "id": "h1",
    "every": 60,
    "metrics": ["soil.moisture", "air.temperature"]
  }]
}
```

## MQTT Topics

All topics prefixed with `jettyd/{tenant_id}/{device_key}/`:

| Direction | Suffix           | Description                      |
|-----------|------------------|----------------------------------|
| Publish   | telemetry        | Periodic sensor readings         |
| Publish   | status           | online, offline, error, sleep    |
| Publish   | command/response | Response to received command     |
| Publish   | shadow/report    | Full device state                |
| Publish   | alert            | Rule-triggered alerts            |
| Subscribe | command          | Incoming commands                |
| Subscribe | config           | JettyScript rule updates         |
| Subscribe | firmware         | OTA update notifications         |
| Subscribe | shadow/desired   | Desired state from platform      |

## Known Gaps

See [TODO.md](TODO.md) for:
- Missing `PUT /devices/{id}/config` platform endpoint
- P1/P2/P3 drivers not yet implemented
- `flash.py` and `simulate.py` tools
- Deep sleep, battery ADC, SNTP time sync
