# soil_moisture — Capacitive / Resistive Moisture Sensor

ADC-based soil moisture sensor. Calibrate `dry_value` and `wet_value` for your sensor.

## Telemetry

| Metric | Type | Unit | Range |
|--------|------|------|-------|
| `{instance}.moisture` | float | % | 0 to 100 |

## Commands

None — read-only sensor.

## Configuration

| Field | Required | Default | Notes |
|-------|----------|---------|-------|
| `pin` | yes | — | ADC-capable GPIO pin |
| `type` | no | `"capacitive"` | `"capacitive"` or `"resistive"` |
| `dry_value` | no | `4095` | Raw ADC at 0% moisture |
| `wet_value` | no | `1200` | Raw ADC at 100% moisture |

```json
{ "driver": "soil_moisture", "instance": "soil", "pin": 34, "dry_value": 3800, "wet_value": 1400 }
```
