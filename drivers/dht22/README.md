# dht22 — Temperature / Humidity Sensor

Single-wire sensor. Minimum read interval 2 seconds per hardware spec.

## Telemetry

| Metric | Type | Unit | Range | Accuracy |
|--------|------|------|-------|----------|
| `{instance}.temperature` | float | °C | −40 to 80 | ±0.5°C |
| `{instance}.humidity` | float | % | 0 to 100 | ±2% |

## Commands

None — read-only sensor.

## Configuration

| Field | Required | Notes |
|-------|----------|-------|
| `pin` | yes | GPIO data pin (single-wire) |

```json
{ "driver": "dht22", "instance": "air", "pin": 4 }
```
