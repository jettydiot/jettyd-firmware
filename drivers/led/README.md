# led — GPIO LED

Simple GPIO LED driver with on, off, toggle, and blink commands.

## Telemetry

| Metric | Type | Notes |
|--------|------|-------|
| `{instance}.state` | bool | true = on, false = off |

## Commands

Send commands via the jettyd commands API or MCP `send_command` tool.

| Action | Parameters | Description |
|--------|-----------|-------------|
| `led.on` | — | Turn LED on |
| `led.off` | — | Turn LED off |
| `led.toggle` | — | Toggle current state |
| `led.blink` | `interval_ms` (default 500), `count` (default 3) | Blink N times |

### Example

```json
{ "action": "led.blink", "params": { "interval_ms": 200, "count": 5 } }
```

## Configuration

| Field | Required | Default | Notes |
|-------|----------|---------|-------|
| `pin` | yes | — | GPIO pin number |
| `active_high` | no | `true` | false = active-low LED |

```json
{ "driver": "led", "instance": "status", "pin": 2, "active_high": true }
```
