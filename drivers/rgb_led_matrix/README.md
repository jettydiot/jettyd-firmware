# rgb_led_matrix — Grove RGB LED Matrix (8×8)

I²C driver for the Grove RGB LED Matrix (STM32F031 + MY9221). Supports named patterns, emoji, hex bitmaps, and color control.

## Telemetry

| Metric | Type | Notes |
|--------|------|-------|
| `{instance}.pattern` | string | Current pattern name or 16-char hex bitmap |
| `{instance}.color` | string | Current color as RGB hex (e.g. `FF0000`) |

## Commands

| Action | Parameters | Description |
|--------|-----------|-------------|
| `matrix.set_pattern` | `pattern` (string), `color` (hex, optional) | Display pattern |
| `matrix.on` | — | Display last pattern |
| `matrix.off` | — | Turn matrix off |

### Named patterns

**Emoji** (fixed colors from STM32 firmware — `color` param ignored):
`smile`, `laugh`, `sad`, `mad`, `angry`, `cry`, `greedy`, `cool`, `shy`, `awkward`,
`heart`, `small_heart`, `broken_heart`, `waterdrop`, `flame`, `sword`, `house`, `tree`,
`flower`, `umbrella`, `rain`, `creeper`, `monster`, `crab`, `duck`, `rabbit`, `cat`,
`up`, `down`, `left`, `right`

**Bitmap** (respects `color` param):
`check`, `x`, `all_on`, `border`, `diamond`, `exclaim`, `question`

**Hex bitmap** (16-char string, respects `color`):
Two hex digits per row, 8 rows. Example: `3C7EFFFFFFFF7E3C` (heart shape).

### Colors

RGB hex string, e.g. `FF0000` (red). Nearest palette color is used automatically.
Available palette: red, orange, yellow, green, cyan, blue, purple, pink, white.

### Examples

```json
{ "action": "matrix.set_pattern", "params": { "pattern": "smile" } }
{ "action": "matrix.set_pattern", "params": { "pattern": "check", "color": "00FF00" } }
{ "action": "matrix.set_pattern", "params": { "pattern": "3C7EFFFFFFFF7E3C", "color": "FF0000" } }
{ "action": "matrix.off" }
```

## Configuration

| Field | Required | Default | Notes |
|-------|----------|---------|-------|
| `sda_pin` | yes | — | GPIO for I²C SDA |
| `scl_pin` | yes | — | GPIO for I²C SCL |
| `i2c_port` | no | `0` | I²C peripheral 0 or 1 |
| `i2c_addr` | no | `0x65` | Grove default address |

```json
{ "driver": "rgb_led_matrix", "instance": "display", "sda_pin": 21, "scl_pin": 22 }
```
