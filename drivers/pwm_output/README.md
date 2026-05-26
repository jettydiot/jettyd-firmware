# pwm_output — PWM Actuator (LEDC)

Variable-duty-cycle PWM output using ESP32 LEDC peripheral. Use for dimmers, buzzers, fan speed control.

## Telemetry

| Metric | Type | Unit | Range |
|--------|------|------|-------|
| `{instance}.duty` | float | % | 0 to 100 |
| `{instance}.freq` | int | Hz | 10 to 40000 |

## Commands

| Action | Parameters | Description |
|--------|-----------|-------------|
| `pwm.set_duty` | `value` (0–100) | Set duty cycle % |
| `pwm.set_freq` | `value` (10–40000) | Set frequency in Hz |
| `pwm.on` | `duration_ms` (optional) | Set duty to 50%, auto-off after duration |
| `pwm.off` | — | Set duty to 0% |

### Example

```json
{ "action": "pwm.set_duty", "params": { "value": 75 } }
```

## Safety

`max_on_duration` auto-off timer prevents indefinite output. Default: 3600s.

## Configuration

| Field | Required | Default | Notes |
|-------|----------|---------|-------|
| `pin` | yes | — | GPIO pin (PWM-capable) |
| `ledc_channel` | no | `0` | LEDC channel 0–7 |
| `freq_hz` | no | `1000` | Initial PWM frequency |
| `max_on_duration` | no | `3600` | Auto-off seconds; 0 = no limit |

```json
{ "driver": "pwm_output", "instance": "buzzer", "pin": 25, "freq_hz": 2000 }
```
