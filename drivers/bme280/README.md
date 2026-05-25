# bme280 — Temperature / Humidity / Pressure Sensor

BME280 I²C sensor. Reads temperature, relative humidity, and barometric pressure.

## Telemetry

| Metric | Type | Unit | Range |
|--------|------|------|-------|
| `{instance}.temperature` | float | °C | −40 to 85 |
| `{instance}.humidity` | float | % | 0 to 100 |
| `{instance}.pressure` | float | hPa | 300 to 1100 |

## Commands

None — read-only sensor.

## Configuration

| Field | Required | Default | Notes |
|-------|----------|---------|-------|
| `i2c_port` | yes | — | I²C peripheral number |
| `sda_pin` | yes | — | GPIO for SDA |
| `scl_pin` | yes | — | GPIO for SCL |
| `i2c_addr` | no | `0x76` | Use `0x77` when SDO pin is high |
| `oversampling` | no | `1` | 1–5 maps to x1–x16 |

```json
{ "driver": "bme280", "instance": "env", "i2c_port": 0, "sda_pin": 21, "scl_pin": 22 }
```
