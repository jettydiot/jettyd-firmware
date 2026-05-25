# hcsr04 — Ultrasonic Distance Sensor

Measures distance by timing an ultrasonic echo pulse. Range: 2 to 400 cm.

## Telemetry

| Metric | Type | Unit | Range |
|--------|------|------|-------|
| `{instance}.distance` | float | cm | 2 to 400 |

## Commands

None — read-only sensor.

## Configuration

| Field | Required | Notes |
|-------|----------|-------|
| `trigger_pin` | yes | GPIO for 10µs trigger pulse |
| `echo_pin` | yes | GPIO for echo pulse measurement |

```json
{ "driver": "hcsr04", "instance": "tank", "trigger_pin": 12, "echo_pin": 13 }
```
