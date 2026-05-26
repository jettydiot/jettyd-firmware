# relay — GPIO Relay

Controls a relay via GPIO. Includes a safety auto-off timer to prevent indefinite activation.

## Telemetry

| Metric | Type | Notes |
|--------|------|-------|
| `{instance}.state` | bool | true = energized |

## Commands

| Action | Parameters | Description |
|--------|-----------|-------------|
| `relay.on` | `duration_ms` (optional) | Energize relay; auto-off after duration |
| `relay.off` | — | De-energize relay, cancel timer |

### Example

```json
{ "action": "relay.on", "params": { "duration_ms": 5000 } }
```

## Safety

`max_on_duration` (default 1800s) ensures the relay cannot stay on indefinitely.

## Configuration

| Field | Required | Default | Notes |
|-------|----------|---------|-------|
| `pin` | yes | — | GPIO pin |
| `active_high` | no | `true` | false = active-low relay |
| `default_state` | no | `"off"` | State on boot: `"on"` or `"off"` |
| `max_on_duration` | no | `1800` | Auto-off seconds; 0 = no limit |

```json
{ "driver": "relay", "instance": "pump", "pin": 26, "max_on_duration": 300 }
```
