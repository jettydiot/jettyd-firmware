# servo — Hobby Servo (LEDC, 50 Hz)

Drives a hobby servo via a LEDC PWM signal. Supports direct angle writes and a
`rotate` command that moves to an angle, holds, then returns to `home_angle`
without blocking the MQTT/command task.

## Telemetry

| Metric | Type | Notes |
|--------|------|-------|
| `{instance}.angle` | float | Current commanded angle in degrees |

## Commands

| Action | Parameters | Description |
|--------|-----------|-------------|
| `rotate` | `angle` (default 90), `hold_ms` (default 1000) | Move to `angle`, hold for `hold_ms`, return to `home_angle` |

### Example

Platform commands address the driver instance as `<instance>.<action>`
(instance is `servo` in the stock `servo-actuator-v1` device):

```json
{ "action": "servo.rotate", "params": { "angle": 120, "hold_ms": 500 } }
```

The `angle` capability can also be written directly via the core `set`
action (no hold/return):

```json
{ "action": "servo.set", "params": { "angle": 45 } }
```

From a rules (JettyScript) `then` block, use the VM's `set_value` action
with the instance as target:

```json
{ "action": "set_value", "target": "servo", "params": { "value": 45 } }
```

## Idle detach

To avoid servo hum/jitter while holding a fixed position, the LEDC signal is
stopped (`ledc_stop`) approximately 500ms after the last commanded move when
`idle_detach` is true (default). The signal automatically re-attaches
(`ledc_channel_config`) on the next move.

## Configuration

| Field | Required | Default | Notes |
|-------|----------|---------|-------|
| `pin` | yes | — | GPIO pin driving the servo signal wire |
| `ledc_channel` | no | `0` | LEDC channel (0-7) |
| `min_pulse_us` | no | `500` | Pulse width at angle 0 |
| `max_pulse_us` | no | `2500` | Pulse width at `max_angle` |
| `max_angle` | no | `180` | Maximum commandable angle in degrees |
| `home_angle` | no | `0` | Angle to return to after a rotate command's hold elapses |
| `idle_detach` | no | `true` | Detach LEDC signal ~500ms after motion completes |

```json
{ "driver": "servo", "instance": "gate", "pin": 2, "max_angle": 180 }
```

## Wiring

Servo signal wire → the configured GPIO pin. Servo V+ → an external 5V
supply (not the ESP32's 3V3 pin — servos draw more current than the onboard
regulator can safely supply). Servo GND → common ground with the ESP32.
