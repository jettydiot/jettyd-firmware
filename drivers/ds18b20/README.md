# ds18b20 — Dallas 1-Wire Temperature Sensor

Dallas 1-Wire protocol. Requires 4.7kΩ pull-up on the data line.

## Telemetry

| Metric | Type | Unit | Range | Accuracy |
|--------|------|------|-------|----------|
| `{instance}.temperature` | float | °C | −55 to 125 | ±0.5°C at 12-bit |

## Commands

None — read-only sensor.

## Configuration

| Field | Required | Default | Notes |
|-------|----------|---------|-------|
| `pin` | yes | — | 1-Wire data pin (needs 4.7kΩ pull-up) |
| `resolution` | no | `12` | 9, 10, 11, or 12 bits |

Conversion time: 94ms (9-bit), 188ms (10-bit), 375ms (11-bit), 750ms (12-bit).

```json
{ "driver": "ds18b20", "instance": "water_temp", "pin": 5, "resolution": 12 }
```
