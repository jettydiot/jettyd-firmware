# button — GPIO Button / Switch

Interrupt-driven GPIO button driver. Detects press, release, long-press, and double-press events.

## Telemetry

| Metric | Type | Unit | Notes |
|--------|------|------|-------|
| `{instance}.press` | bool | — | true on press, false on release |
| `{instance}.press_count` | float | count | Cumulative press counter |
| `{instance}.long_press` | bool | — | true when held > `long_press_ms` |
| `{instance}.double_press` | bool | — | true on double-tap within `double_press_ms` |

## Commands

None — event-driven sensor.

## Configuration

| Field | Required | Default | Notes |
|-------|----------|---------|-------|
| `pin` | yes | — | GPIO pin number |
| `active_low` | no | `true` | true = internal pull-up enabled |
| `debounce_ms` | no | `50` | Edge debounce window in ms |
| `long_press_ms` | no | `500` | Hold duration for long press; 0 = disabled |
| `double_press_ms` | no | `300` | Max gap between presses; 0 = disabled |

```json
{ "driver": "button", "instance": "btn", "pin": 0, "active_low": true }
```
