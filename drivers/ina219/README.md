# ina219 — Voltage / Current / Power Monitor

I²C power monitor. Measures bus voltage, shunt current, and calculated power.

## Telemetry

| Metric | Type | Unit | Range |
|--------|------|------|-------|
| `{instance}.voltage` | float | V | 0 to 26 |
| `{instance}.current` | float | A | −3.2 to 3.2 |
| `{instance}.power` | float | W | 0 to 83.2 |

## Commands

None — read-only sensor.

## Configuration

| Field | Required | Default | Notes |
|-------|----------|---------|-------|
| `i2c_port` | yes | — | I²C peripheral number |
| `sda_pin` | yes | — | GPIO for SDA |
| `scl_pin` | yes | — | GPIO for SCL |
| `i2c_addr` | no | `0x40` | Set by A0/A1 pins |
| `shunt_resistance_mohm` | no | `100` | Shunt value in milliohms |

```json
{ "driver": "ina219", "instance": "solar", "i2c_port": 0, "sda_pin": 21, "scl_pin": 22 }
```
