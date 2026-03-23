# Jettyd Firmware SDK — Developer Specification

Version: 1.0-draft
Status: For implementation
Platform: ESP-IDF v5.5+
Language: C (ESP-IDF standard)
Repository: `apps/firmware/` in the jettyd monorepo

---

## 1. Architecture overview

The Jettyd firmware has three layers. Each layer has a clear boundary
and a defined interface to the layers above and below it.

```
┌──────────────────────────────────────────────────┐
│  Layer 3: JettyScript rule VM                    │  ← Runtime, received over MQTT
│  Evaluates rules, schedules heartbeats           │     No reflash needed
│  Stored in NVS, persists across reboots          │     Pushed by platform/agent
├──────────────────────────────────────────────────┤
│  Layer 2: Jettyd core runtime                    │  ← Always compiled in
│  WiFi, MQTT, OTA, provisioning, shadow, NVS      │     Fixed for all devices
│  Heartbeat, watchdog, telemetry publisher         │
├──────────────────────────────────────────────────┤
│  Layer 1: Peripheral drivers                     │  ← Compile-time selection
│  Sensor reads, actuator controls                 │     Only selected drivers linked
│  Each driver registers capabilities at boot      │     From driver library
└──────────────────────────────────────────────────┘
```

**Design principles:**
- Thin device, smart platform. The device does as little as possible.
- Compile-time hardware, runtime logic. Peripherals are fixed; rules are hot-swappable.
- Every driver speaks the same abstract interface. The VM doesn't know what hardware it's running on.
- Fail safe. If rules are invalid, the device falls back to heartbeat-only mode.
- Bounded resource usage. No dynamic allocation in the rule VM. Fixed limits on everything.

---

## 2. Project structure

```
apps/firmware/
├── core/                          # Layer 2: always compiled
│   ├── include/
│   │   ├── jettyd.h               # Top-level API: jettyd_init(), jettyd_start()
│   │   ├── jettyd_mqtt.h          # MQTT client interface
│   │   ├── jettyd_ota.h           # OTA update interface
│   │   ├── jettyd_provision.h     # Fleet provisioning interface
│   │   ├── jettyd_shadow.h        # Device shadow (local state cache)
│   │   ├── jettyd_nvs.h           # NVS abstraction
│   │   ├── jettyd_wifi.h          # WiFi management
│   │   ├── jettyd_telemetry.h     # Telemetry publishing
│   │   ├── jettyd_vm.h            # JettyScript rule VM interface
│   │   └── jettyd_driver.h        # Driver registration interface
│   ├── src/
│   │   ├── jettyd.c
│   │   ├── mqtt.c
│   │   ├── ota.c
│   │   ├── provision.c
│   │   ├── shadow.c
│   │   ├── nvs.c
│   │   ├── wifi.c
│   │   ├── telemetry.c
│   │   ├── vm.c                   # JettyScript evaluator
│   │   └── driver_registry.c      # Runtime driver discovery
│   └── CMakeLists.txt
│
├── drivers/                       # Layer 1: peripheral library
│   ├── soil_moisture/
│   │   ├── include/soil_moisture.h
│   │   ├── soil_moisture.c
│   │   ├── driver.yaml            # Driver metadata
│   │   └── CMakeLists.txt
│   ├── dht22/
│   ├── ds18b20/
│   ├── bme280/
│   ├── bh1750/
│   ├── hcsr04/
│   ├── ina219/
│   ├── relay/
│   ├── pwm_output/
│   ├── servo/
│   ├── solenoid_valve/
│   ├── buzzer/
│   ├── gps_nmea/
│   ├── hx711/
│   ├── mq_gas/
│   ├── modbus_rtu/
│   └── ble_scanner/
│
├── devices/                       # Device type configurations
│   ├── soil-moisture-v1/
│   │   ├── device.yaml
│   │   └── main.c                 # Minimal: calls jettyd_init() + jettyd_start()
│   ├── env-monitor-v1/
│   │   ├── device.yaml
│   │   └── main.c
│   └── relay-controller-v1/
│       ├── device.yaml
│       └── main.c
│
├── tools/
│   ├── build.py                   # Reads device.yaml, generates build config
│   ├── flash.py                   # Flash + provision helper
│   └── simulate.py                # Software simulator for testing
│
├── test/
│   ├── test_vm.c                  # Unit tests for rule VM
│   ├── test_drivers.c             # Unit tests for driver interface
│   └── test_telemetry.c
│
└── CMakeLists.txt                 # Top-level build, delegates to device config
```

---

## 3. Device configuration (device.yaml)

Each device type is defined by a `device.yaml` that specifies which
drivers to include and how they're wired. The build system reads this
file and generates the appropriate ESP-IDF configuration.

### Schema

```yaml
# devices/soil-moisture-v1/device.yaml

name: "soil-moisture-v1"               # Must match device type slug on platform
version: "1.0.0"                        # Firmware version
target: "esp32s3"                       # esp32s3 | esp32c3 | esp32c6
description: "Soil moisture sensor with irrigation relay"

# Peripheral drivers to compile in
drivers:
  - name: soil_moisture
    instance: "soil"                    # Namespace in rules/telemetry
    config:
      pin: 34
      type: "capacitive"               # capacitive | resistive
      dry_value: 4095                   # ADC reading when dry
      wet_value: 1200                   # ADC reading when submerged
      unit: "%"

  - name: dht22
    instance: "air"
    config:
      pin: 4

  - name: relay
    instance: "valve"
    config:
      pin: 25
      active_high: true                 # true = HIGH to activate
      default_state: "off"
      max_on_duration: 1800             # Safety: auto-off after 30 min

# Default heartbeat (active before any rules are pushed)
defaults:
  heartbeat_interval: 60                # Seconds between telemetry reports
  report_metrics:                       # What to include in heartbeat
    - "soil.moisture"
    - "air.temperature"
    - "air.humidity"
    - "valve.state"
    - "system.battery"
    - "system.rssi"

# Connection
mqtt:
  keepalive: 60
  qos: 1
  buffer_on_disconnect: true            # Buffer messages if MQTT disconnects
  max_buffer_size: 50                   # Max buffered messages

# Power management (optional)
power:
  deep_sleep: false                     # If true, device sleeps between heartbeats
  sleep_duration: null                  # Seconds of deep sleep (overrides heartbeat_interval)
  wake_on_pin: null                     # GPIO that triggers wake from sleep

# Hardware
hardware:
  has_battery: true
  battery_adc_pin: 35
  battery_voltage_divider: 2.0          # Multiply ADC reading by this
  status_led_pin: 2                     # Optional: blink on activity
```

### Build process

The build tool (`tools/build.py`) reads `device.yaml` and:

1. Resolves driver dependencies (each driver's `driver.yaml` may declare
   ESP-IDF component dependencies)
2. Generates `sdkconfig.defaults` with appropriate partition table, flash
   size, and component selections
3. Generates `driver_init.c` — auto-generated code that calls each
   driver's init function with the config from `device.yaml`
4. Generates `driver_manifest.h` — compile-time constant listing all
   driver instances and their capabilities
5. Invokes `idf.py build`

**Build command:**
```bash
cd apps/firmware
python tools/build.py --device soil-moisture-v1 --target esp32s3
# Outputs: devices/soil-moisture-v1/build/firmware.bin
```

**Flash + provision command:**
```bash
python tools/flash.py \
  --device soil-moisture-v1 \
  --port /dev/ttyUSB0 \
  --tenant-id tn_abc123 \
  --fleet-token ft_live_xxxxx
```

This flashes the firmware and writes the provisioning config to the
NVS partition in a single operation.

---

## 4. Driver interface (Layer 1)

Every peripheral driver implements the same abstract interface. The
core runtime discovers drivers at boot via a registration table.

### Driver registration

```c
// core/include/jettyd_driver.h

#define JETTYD_MAX_DRIVERS 16
#define JETTYD_MAX_CAPABILITIES 8
#define JETTYD_MAX_INSTANCE_NAME 16

typedef enum {
    JETTYD_CAP_READABLE,       // Can be read (sensor value)
    JETTYD_CAP_WRITABLE,       // Can be set to a value (PWM, servo)
    JETTYD_CAP_SWITCHABLE,     // Can be toggled on/off (relay, valve)
    JETTYD_CAP_ALERTABLE,      // Can generate threshold alerts
} jettyd_capability_type_t;

typedef enum {
    JETTYD_VAL_FLOAT,
    JETTYD_VAL_INT,
    JETTYD_VAL_BOOL,
    JETTYD_VAL_STRING,
} jettyd_value_type_t;

typedef struct {
    char name[32];                      // e.g., "moisture", "temperature"
    jettyd_capability_type_t type;
    jettyd_value_type_t value_type;
    float min_value;                    // For validation
    float max_value;
    char unit[8];                       // e.g., "%", "°C", "V"
} jettyd_capability_t;

typedef struct {
    float float_val;
    int32_t int_val;
    bool bool_val;
    char str_val[32];
    jettyd_value_type_t type;
    bool valid;                         // false if read failed
} jettyd_value_t;

typedef struct {
    // Identity
    char instance[JETTYD_MAX_INSTANCE_NAME];  // e.g., "soil", "valve"
    char driver_name[32];                      // e.g., "soil_moisture", "relay"

    // Capabilities this instance provides
    jettyd_capability_t capabilities[JETTYD_MAX_CAPABILITIES];
    uint8_t capability_count;

    // Lifecycle
    esp_err_t (*init)(const void *config);     // Called once at boot
    esp_err_t (*deinit)(void);                 // Called on shutdown

    // Operations
    jettyd_value_t (*read)(const char *capability_name);
    esp_err_t (*write)(const char *capability_name, jettyd_value_t value);
    esp_err_t (*switch_on)(uint32_t duration_ms);   // 0 = indefinite
    esp_err_t (*switch_off)(void);
    bool (*get_state)(void);                        // For switchable: is it on?

    // Optional
    esp_err_t (*calibrate)(const char *type);       // e.g., "dry", "wet"
    esp_err_t (*self_test)(void);                   // Returns OK if hardware responds
} jettyd_driver_t;

// Registration macro — called in each driver's init
#define JETTYD_REGISTER_DRIVER(driver_ptr) \
    jettyd_driver_registry_add(driver_ptr)

// Registry functions
esp_err_t jettyd_driver_registry_add(const jettyd_driver_t *driver);
const jettyd_driver_t* jettyd_driver_find(const char *instance);
const jettyd_driver_t* jettyd_driver_find_capability(const char *dotted_name);
uint8_t jettyd_driver_count(void);
const jettyd_driver_t* jettyd_driver_get(uint8_t index);
```

### Example driver implementation

```c
// drivers/soil_moisture/soil_moisture.c

#include "jettyd_driver.h"
#include "driver/adc.h"

typedef struct {
    uint8_t pin;
    uint16_t dry_value;
    uint16_t wet_value;
    adc_channel_t channel;
} soil_config_t;

static soil_config_t cfg;
static jettyd_driver_t driver;

static esp_err_t soil_init(const void *config) {
    const soil_config_t *c = (const soil_config_t *)config;
    cfg = *c;
    // Configure ADC
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(cfg.channel, ADC_ATTEN_DB_11);
    return ESP_OK;
}

static jettyd_value_t soil_read(const char *cap) {
    jettyd_value_t val = { .type = JETTYD_VAL_FLOAT, .valid = true };
    int raw = adc1_get_raw(cfg.channel);
    // Map raw ADC to percentage
    float pct = 100.0f * (float)(cfg.dry_value - raw)
                / (float)(cfg.dry_value - cfg.wet_value);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    val.float_val = pct;
    return val;
}

// Called by auto-generated driver_init.c
void soil_moisture_register(const char *instance, const void *config) {
    soil_init(config);

    memset(&driver, 0, sizeof(driver));
    strncpy(driver.instance, instance, JETTYD_MAX_INSTANCE_NAME);
    strncpy(driver.driver_name, "soil_moisture", 32);

    driver.capability_count = 1;
    strncpy(driver.capabilities[0].name, "moisture", 32);
    driver.capabilities[0].type = JETTYD_CAP_READABLE;
    driver.capabilities[0].value_type = JETTYD_VAL_FLOAT;
    driver.capabilities[0].min_value = 0;
    driver.capabilities[0].max_value = 100;
    strncpy(driver.capabilities[0].unit, "%", 8);

    driver.init = soil_init;
    driver.read = soil_read;

    JETTYD_REGISTER_DRIVER(&driver);
}
```

### Driver metadata (driver.yaml)

Each driver includes a metadata file for the build system and
documentation:

```yaml
# drivers/soil_moisture/driver.yaml

name: soil_moisture
description: "Capacitive or resistive soil moisture sensor via ADC"
category: sensor
capabilities:
  - name: moisture
    type: readable
    value_type: float
    unit: "%"
    description: "Soil moisture percentage (0=dry, 100=saturated)"

config_schema:
  pin:
    type: int
    required: true
    description: "ADC-capable GPIO pin number"
  type:
    type: string
    enum: [capacitive, resistive]
    default: capacitive
  dry_value:
    type: int
    default: 4095
    description: "Raw ADC reading when sensor is completely dry"
  wet_value:
    type: int
    default: 1200
    description: "Raw ADC reading when sensor is submerged in water"
  unit:
    type: string
    default: "%"

esp_idf_components:
  - driver    # ESP-IDF ADC driver

compatible_targets:
  - esp32s3
  - esp32c3
  - esp32c6
```

### Driver library (initial set)

Implement these drivers for v1, in priority order:

| Driver         | Category | Capabilities          | Interface | Priority |
|----------------|----------|-----------------------|-----------|----------|
| relay          | actuator | switchable            | GPIO      | P0       |
| soil_moisture  | sensor   | moisture (float %)    | ADC       | P0       |
| dht22          | sensor   | temperature, humidity  | OneWire   | P0       |
| ds18b20        | sensor   | temperature           | OneWire   | P0       |
| bme280         | sensor   | temp, humidity, pressure | I2C    | P1       |
| pwm_output     | actuator | writable (0-100%)     | LEDC/PWM  | P1       |
| hcsr04         | sensor   | distance (cm)         | GPIO trig/echo | P1  |
| ina219         | sensor   | voltage, current, power | I2C     | P1       |
| bh1750         | sensor   | light (lux)           | I2C       | P2       |
| gps_nmea       | sensor   | latitude, longitude, altitude | UART | P2  |
| hx711          | sensor   | weight (g)            | GPIO clk/data | P2  |
| mq_gas         | sensor   | gas_level (ppm)       | ADC       | P2       |
| solenoid_valve | actuator | switchable + timed    | GPIO      | P2       |
| servo          | actuator | writable (angle 0-180) | PWM     | P2       |
| buzzer         | actuator | switchable + tone     | PWM       | P2       |
| modbus_rtu     | bridge   | configurable registers | UART/RS485 | P3   |
| ble_scanner    | bridge   | BLE advertisement data | BLE      | P3       |

---

## 5. Core runtime (Layer 2)

### Boot sequence

```
1.  Hardware init (ESP-IDF boot)
2.  NVS init
3.  Read provisioning state from NVS
4.  WiFi connect (with retry + exponential backoff)
5.  If not provisioned:
      a. Connect to MQTT with fleet token
      b. Publish to jettyd/provision/request
      c. Wait for device key on jettyd/provision/response/{device_key}
      d. Store device key in NVS
      e. Delete fleet token from NVS
      f. Reconnect to MQTT with device key
6.  If provisioned:
      a. Connect to MQTT with device key
7.  Register all compiled-in drivers (auto-generated driver_init.c)
8.  Publish device manifest (capabilities list) to shadow
9.  Load JettyScript rules from NVS
10. Start rule VM
11. Start heartbeat timer
12. Subscribe to command topic
13. Subscribe to config topic (for rule updates)
14. Subscribe to firmware topic (for OTA)
15. Enter main loop
```

### MQTT topic structure

All topics are prefixed with `jettyd/{tenant_id}/{device_key}/`.

```
# Device → Platform (PUBLISH)
.../telemetry          Periodic sensor readings (heartbeat)
.../status             Device status: online, offline, error, sleep
.../command/response   Response to a received command
.../shadow/report      Full shadow state report
.../alert              Rule-triggered alert

# Platform → Device (SUBSCRIBE)
.../command            Incoming commands from platform/agent
.../config             JettyScript rule + heartbeat configuration
.../firmware           OTA update notification
.../shadow/desired     Desired state from platform

# Provisioning (special, pre-auth)
jettyd/provision/request                 Fleet token + device info
jettyd/provision/response/{device_key}   Device credentials
```

### Telemetry message format

```json
{
  "ts": 1710763200,
  "readings": {
    "soil.moisture": 42.3,
    "air.temperature": 23.1,
    "air.humidity": 65.2,
    "valve.state": false,
    "system.battery": 3.82,
    "system.rssi": -54,
    "system.uptime": 86400,
    "system.heap_free": 128000
  }
}
```

Field naming convention: `{instance}.{capability}`. This matches
how JettyScript rules reference values.

System readings are always included: `system.battery`, `system.rssi`,
`system.uptime`, `system.heap_free`. These help the platform assess
device health.

### Command handling

Commands arrive on the `command` topic:

```json
{
  "id": "cmd_abc123",
  "action": "valve.on",
  "params": {
    "duration": 300
  },
  "timeout": 30
}
```

The core runtime:
1. Parses the command
2. Finds the driver instance ("valve")
3. Validates the action against driver capabilities
4. Executes the action
5. Publishes response on `command/response`:

```json
{
  "id": "cmd_abc123",
  "status": "acked",
  "result": {
    "valve.state": true,
    "will_auto_off_at": 1710763500
  }
}
```

Status values: `acked` (executed successfully), `failed` (execution
error), `rejected` (invalid action or params), `busy` (device is
processing another command).

### OTA update flow

1. Platform publishes to `firmware` topic:
   ```json
   {
     "version": "1.3.0",
     "url": "https://r2.jettyd.com/firmware/soil-moisture-v1/v1.3.0/esp32s3/firmware.bin",
     "checksum": "sha256:a1b2c3...",
     "size": 524288
   }
   ```
2. Device compares version to current — skips if already running this version
3. Device downloads firmware binary over HTTPS
4. Verifies SHA256 checksum
5. Writes to OTA partition
6. Sets boot partition to new firmware
7. Publishes status: `{"status": "updating", "version": "1.3.0"}`
8. Reboots
9. On successful boot, publishes: `{"status": "updated", "version": "1.3.0"}`
10. If boot fails (watchdog triggers), ESP-IDF rolls back to previous partition

### Shadow management

The device maintains a local shadow — a JSON document representing
its current state. The shadow is updated whenever:

- A sensor is read (heartbeat or rule-triggered)
- An actuator state changes
- The device connects/disconnects
- A rule is loaded or removed

The shadow is published to `shadow/report` on connect and on every
state change. The platform can push a `shadow/desired` to request
a state change (e.g., turn on a relay). The device reconciles
desired vs reported state.

Shadow structure:
```json
{
  "reported": {
    "soil.moisture": 42.3,
    "air.temperature": 23.1,
    "valve.state": false,
    "system.firmware_version": "1.2.0",
    "system.uptime": 86400,
    "vm.rules_loaded": 2,
    "vm.heartbeats_loaded": 1
  },
  "desired": null,
  "metadata": {
    "last_report": 1710763200
  }
}
```

---

## 6. JettyScript rule VM (Layer 3)

### Overview

JettyScript is a declarative JSON-based DSL for on-device automation.
Rules and heartbeats are sent to the device over MQTT on the `config`
topic, validated against compiled-in driver capabilities, stored in
NVS, and executed by a lightweight virtual machine.

JettyScript is NOT a programming language. It is a structured
configuration format with a fixed set of operations. There are no
variables, no loops, no user-defined functions.

### Config message format

```json
{
  "version": 1,
  "rules": [ ... ],
  "heartbeats": [ ... ]
}
```

Sending a new config **replaces** all existing rules and heartbeats.
To add a rule, the sender must include all existing rules plus the
new one. This is intentional — it prevents state divergence between
the platform and the device.

### Rule format

```json
{
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
  "then": [
    {
      "action": "switch_on",
      "target": "valve",
      "params": { "duration": 300 }
    },
    {
      "action": "alert",
      "params": {
        "message": "Irrigation triggered: moisture at {{soil.moisture}}%",
        "severity": "info"
      }
    }
  ]
}
```

### Condition types

```
threshold
  Fires when a sensor reading crosses a boundary.
  Fields: sensor, op, value, debounce (seconds)
  Operators: < > <= >= == !=
  Only fires on CROSSING — not on every read that matches.
  Debounce prevents re-triggering within N seconds.

range
  Fires when a reading enters or exits a range.
  Fields: sensor, min, max, on_enter (bool), on_exit (bool), debounce

compound
  Combines multiple conditions with and/or.
  Fields: operator ("and" | "or"), conditions (array of conditions)
  Max nesting depth: 2

schedule
  Fires at a specific time (requires device to have RTC or NTP sync).
  Fields: cron (simplified: "HH:MM" or "HH:MM weekday_bitmask")
  Weekday bitmask: 0b1111100 = Mon-Fri, 0b0000011 = Sat-Sun

time_window
  Modifier: rule only active during specified hours.
  Fields: start_hour, end_hour, weekday_bitmask
  Applied as an AND condition on any trigger type.
```

### Action types

```
switch_on
  Turn on a switchable actuator.
  Target: driver instance name (e.g., "valve")
  Params:
    duration (int, seconds) — auto-off after N seconds. 0 = indefinite.
    Max duration enforced by driver's max_on_duration config.

switch_off
  Turn off a switchable actuator.
  Target: driver instance name.

set_value
  Set a writable actuator to a value.
  Target: driver instance name (e.g., "dimmer")
  Params:
    value (float) — validated against driver's min/max range.

report
  Immediately send a telemetry reading (outside heartbeat schedule).
  Params:
    metrics (array of strings) — e.g., ["soil.moisture", "valve.state"]
    If empty, reports all available metrics.

alert
  Send an alert message to the platform.
  Params:
    message (string) — supports {{sensor.name}} template substitution.
    severity ("info" | "warning" | "critical")

sleep
  Pause between chained actions.
  Params:
    seconds (int) — max 60.

set_heartbeat
  Dynamically change reporting interval.
  Params:
    interval (int, seconds) — min 10, max 3600.
    Used for adaptive reporting (e.g., increase frequency during alert).
```

### Heartbeat format

```json
{
  "id": "h1",
  "every": 60,
  "metrics": [
    "soil.moisture",
    "air.temperature",
    "air.humidity",
    "valve.state"
  ]
}
```

Multiple heartbeats can coexist at different intervals. Example:
- h1: every 60s, report soil + air (high priority)
- h2: every 3600s, report battery + rssi + uptime (housekeeping)

### Template substitution in alerts

Alert messages support `{{instance.capability}}` placeholders that
are replaced with current readings at evaluation time:

```json
{
  "action": "alert",
  "params": {
    "message": "Low moisture: {{soil.moisture}}% at {{air.temperature}}°C",
    "severity": "warning"
  }
}
```

### Validation rules

When the device receives a new config, the VM validates:

1. **Version check:** `version` field must be 1.
2. **Rule count:** Max 16 rules, max 8 heartbeats.
3. **Capability check:** Every sensor/target reference must match a
   registered driver instance + capability. If `soil.moisture` is
   referenced but no driver instance "soil" exists, the rule is
   rejected.
4. **Type check:** Threshold values must be numeric for numeric
   capabilities. Switch actions must target switchable drivers.
5. **Safety check:** Duration params cannot exceed the driver's
   `max_on_duration`. Heartbeat intervals must be 10-3600 seconds.
6. **Size check:** Total serialised config must fit in NVS
   allocation (default 4KB).

If ANY rule fails validation, the ENTIRE config is rejected and
the device continues running the previous config. The device
publishes a rejection message:

```json
{
  "type": "config_rejected",
  "errors": [
    {"rule": "r1", "error": "Unknown sensor: soil.ph"},
    {"rule": "r3", "error": "Duration 7200 exceeds max 1800 for valve"}
  ]
}
```

### VM execution model

The VM runs as a FreeRTOS task at medium priority. It does not use
dynamic memory allocation — all rule state is stored in a fixed-size
array allocated at boot.

```c
#define JETTYD_VM_MAX_RULES 16
#define JETTYD_VM_MAX_HEARTBEATS 8

typedef struct {
    char id[8];
    bool enabled;
    jettyd_condition_t condition;
    jettyd_action_t actions[4];         // Max 4 actions per rule
    uint8_t action_count;
    int64_t last_triggered_us;          // For debounce
    bool last_condition_state;          // For edge detection
} jettyd_rule_t;

typedef struct {
    char id[8];
    uint32_t interval_sec;
    char metrics[8][32];                // Max 8 metrics per heartbeat
    uint8_t metric_count;
    int64_t last_fired_us;
} jettyd_heartbeat_t;

typedef struct {
    jettyd_rule_t rules[JETTYD_VM_MAX_RULES];
    uint8_t rule_count;
    jettyd_heartbeat_t heartbeats[JETTYD_VM_MAX_HEARTBEATS];
    uint8_t heartbeat_count;
    bool loaded;
} jettyd_vm_state_t;
```

**Evaluation loop (runs every 100ms):**
```
for each heartbeat:
    if time_since_last_fired >= interval:
        read specified metrics
        publish telemetry message
        update last_fired

for each enabled rule:
    evaluate condition:
        read sensor value from driver
        compare against threshold/range
    
    if condition is TRUE and was FALSE last tick (edge detect):
        if time_since_last_triggered >= debounce:
            for each action in rule.actions:
                execute action
            update last_triggered
    
    update last_condition_state
```

The 100ms tick rate means rules respond within 100ms of a sensor
crossing a threshold. This is fast enough for irrigation, HVAC,
alert generation, and most industrial monitoring use cases.

### NVS storage

The JettyScript config is stored in NVS under key `jettyd_vm_config`
as a serialised JSON blob. On boot, the VM reads this key and loads
the rules. If the key doesn't exist or is corrupt, the device runs
with defaults from `device.yaml` (heartbeat-only, no rules).

NVS partition allocation:
- `jettyd_prov`: 4KB — provisioning data (tenant, device key, fleet token)
- `jettyd_vm`: 4KB — JettyScript config
- `jettyd_shadow`: 2KB — last shadow state
- `jettyd_ota`: 1KB — OTA state (target version, retry count)

---

## 7. Platform integration

### How rules get to the device

```
Agent/Dashboard → POST /devices/:id/config → Platform stores in DB
                                            → Platform publishes to MQTT
                                            → Device receives on config topic
                                            → VM validates and loads
                                            → Device publishes ack/reject
                                            → Platform updates device record
```

### API endpoint

```
PUT /devices/{device_id}/config
Authorization: Bearer tk_xxx

{
  "version": 1,
  "rules": [ ... ],
  "heartbeats": [ ... ]
}
```

The platform:
1. Validates the config against the device type's capability schema
2. Stores the config in the device record
3. Publishes to MQTT `jettyd/{tenant}/{device}/config`
4. Returns 202 Accepted (async — device may be offline)

If the device is offline, the platform stores the config and pushes
it when the device reconnects (via MQTT retained message or on-connect
push).

### OpenClaw integration

The OpenClaw skill exposes a `configure_device` tool:

```
Tool: configure_device
Description: Set up automation rules on a device.
Parameters:
  device_id (string): The device to configure
  rules (array): Rules in JettyScript format
  heartbeats (array): Heartbeat schedules
```

Example agent conversation:
  User: "Make the greenhouse water itself when it's dry"
  Agent: → reads device capabilities via get_device
  Agent: → sees soil.moisture (readable) + valve (switchable)
  Agent: → generates JettyScript rule
  Agent: → calls configure_device with the rule
  Agent: "Done. When soil moisture drops below 30%, the valve will
          open for 5 minutes. Telemetry reports every 60 seconds."

### Dashboard UI

The dashboard has a visual rule builder that generates JettyScript:

1. User selects device → dashboard fetches device capabilities
2. Builder shows available sensors and actuators as drag-drop blocks
3. User constructs: WHEN [soil.moisture] [<] [30] THEN [valve ON] [5 min]
4. Dashboard serialises to JettyScript JSON
5. Sends via PUT /devices/:id/config

---

## 8. Safety and constraints

### Resource limits (enforced by VM)

| Resource                    | Limit | Rationale                         |
|-----------------------------|-------|-----------------------------------|
| Rules per device            | 16    | Fixed array, no dynamic alloc     |
| Heartbeats per device       | 8     | Fixed array                       |
| Actions per rule            | 4     | Prevents runaway action chains    |
| Condition nesting depth     | 2     | Keeps evaluation bounded          |
| Max action duration         | Per-driver max_on_duration | Safety |
| Min heartbeat interval      | 10s   | Prevents MQTT flooding            |
| Max heartbeat interval      | 3600s | Ensures device checks in hourly   |
| Max sleep between actions   | 60s   | Prevents task blocking            |
| Config size (serialised)    | 4KB   | NVS partition limit               |
| Template substitution depth | 1     | No nested templates               |

### Actuator safety

Every switchable driver must declare `max_on_duration` in its config.
If a rule or command tries to keep an actuator on longer than this,
the driver's internal timer overrides and turns it off. This prevents:
- A stuck relay leaving a pump running indefinitely
- A solenoid valve flooding a field
- A heater running dangerously long

The `max_on_duration` is a compile-time safety net. The platform can
set shorter durations via rules, but never longer.

### Watchdog

The ESP-IDF task watchdog monitors the main loop. If the VM evaluation
loop takes longer than 5 seconds (stuck sensor read, I2C bus hang),
the watchdog triggers a restart. On restart, the device reconnects
and resumes with the last valid config from NVS.

### Fail-safe mode

If the device cannot parse its stored JettyScript config (corrupt NVS,
incompatible after firmware update), it enters fail-safe mode:

1. All actuators set to default state (from device.yaml)
2. Heartbeat at the default interval (from device.yaml)
3. Publishes alert: `{"type": "failsafe", "reason": "config_parse_error"}`
4. Waits for a new config push from the platform

---

## 9. Testing

### Unit tests (host-side, no hardware)

```bash
cd apps/firmware
idf.py --target linux build   # ESP-IDF Linux target
./build/test_vm               # Run VM unit tests
./build/test_drivers           # Run driver interface tests
```

Test the VM with mock drivers:
- Load valid config → verify rules execute
- Load config referencing non-existent driver → verify rejection
- Verify debounce: trigger condition twice within debounce window → only fires once
- Verify edge detection: condition stays true → only fires on first crossing
- Verify max_on_duration: request 1h on, max is 30m → auto-off at 30m
- Verify NVS persistence: load config, simulated reboot, verify config reloaded
- Verify config replacement: load config A, then config B → only B's rules active

### Integration tests (on hardware)

Use the `tools/simulate.py` script to run a software simulator that
behaves like a real device: connects to the staging MQTT broker, sends
telemetry, receives commands, and evaluates rules. The CI pipeline
runs this against the staging platform nightly.

### Hardware test checklist

For each new driver, test on physical hardware:
- [ ] Driver init succeeds on target chip
- [ ] Read returns valid values in expected range
- [ ] Write/switch operations work
- [ ] Self-test passes with hardware connected
- [ ] Self-test fails gracefully with hardware disconnected
- [ ] Driver handles I2C/SPI bus errors without crashing
- [ ] Power consumption measured and documented

---

## 10. Contributing a new driver

To add a new peripheral to the driver library:

1. Create directory: `drivers/{driver_name}/`
2. Create `driver.yaml` with metadata and config schema
3. Implement the `jettyd_driver_t` interface in `{driver_name}.c`
4. Implement the registration function:
   `void {driver_name}_register(const char *instance, const void *config)`
5. Write unit tests using mock hardware
6. Test on physical hardware with at least one target chip
7. Add entry to the driver table in this spec
8. Submit PR

The build system automatically discovers drivers by scanning the
`drivers/` directory. No changes to the build system are needed.

### Driver checklist

- [ ] Implements read() OR switch_on()/switch_off() OR write()
- [ ] Declares all capabilities in driver.yaml
- [ ] Config schema matches constructor parameters
- [ ] Handles hardware absence gracefully (returns invalid reading, not crash)
- [ ] Respects max_on_duration for switchable drivers
- [ ] No dynamic memory allocation after init
- [ ] No blocking calls longer than 100ms (use async I2C/SPI if needed)
- [ ] Tested on at least one physical target
- [ ] driver.yaml lists compatible_targets

---

## 11. Memory budget

Target: firmware fits on ESP32-C3 (4MB flash, 400KB SRAM, no PSRAM).

| Component                  | Flash estimate | RAM estimate |
|----------------------------|---------------|-------------|
| ESP-IDF base (WiFi, TLS)  | ~1.2MB        | ~120KB      |
| MQTT client (TLS)          | ~80KB         | ~20KB       |
| Jettyd core runtime        | ~60KB         | ~16KB       |
| JettyScript VM             | ~20KB         | ~8KB        |
| NVS storage                | ~16KB         | ~4KB        |
| OTA client                 | ~30KB         | ~8KB        |
| Per driver (average)       | ~10KB         | ~2KB        |
| 5 drivers compiled in      | ~50KB         | ~10KB       |
| **Total**                  | **~1.5MB**    | **~186KB**  |
| **Available on ESP32-C3**  | 4MB           | 400KB       |
| **Headroom**               | ~2.5MB        | ~214KB      |

This leaves ample room for OTA dual-partition (two firmware slots),
additional drivers, and NVS storage.

---

## 12. Versioning and compatibility

### Firmware version

Format: `{major}.{minor}.{patch}` (semver).

- Major: breaking changes to JettyScript format or MQTT protocol
- Minor: new drivers, new VM features (backward compatible)
- Patch: bug fixes

### JettyScript version

The `version` field in the config payload. Currently `1`. If the VM
format changes in a backward-incompatible way, this increments. The
device rejects configs with a version higher than it supports.

### Driver API version

The `jettyd_driver_t` struct is versioned. If fields are added, a
version field at the top of the struct allows the registry to handle
old and new drivers in the same binary.

### OTA compatibility

The platform tracks `device_type_id` and `hardware_target` for each
firmware version. A firmware built for `soil-moisture-v1` on `esp32s3`
will not be offered to an `env-monitor-v1` device or an `esp32c3`
target. The device also checks the firmware type tag in the OTA
header before applying.
