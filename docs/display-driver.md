# MAX7219 LED Matrix Display Driver

Driver for the common FC16-style 4-module MAX7219 LED matrix kit (32×8 pixels).
Exposes the `display.set` command via the standard jettyd command/response path.

## Wiring

| MAX7219 module | ESP32 pin | Kconfig symbol | Default |
|----------------|-----------|----------------|---------|
| VCC | 5V | — | — |
| GND | GND | — | — |
| DIN | GPIO **10** | `JETTYD_DISPLAY_PIN_DIN` | 10 |
| CLK | GPIO **8** | `JETTYD_DISPLAY_PIN_CLK` | 8 |
| CS | GPIO **9** | `JETTYD_DISPLAY_PIN_CS` | 9 |

> **Note:** the FC16 reference wiring diagram labels CS as GPIO20, but the
> reference sketch code connects it to GPIO9. The driver defaults to GPIO9;
> set `JETTYD_DISPLAY_PIN_CS` to 20 (or any other pin) as needed.

Daisy-chain additional modules by connecting the first module's DOUT to the
second module's DIN. The driver supports 1–4 modules
(`JETTYD_DISPLAY_MODULES`, default 4).

## Kconfig options

| Symbol | Default | Description |
|--------|---------|-------------|
| `JETTYD_DISPLAY_PIN_DIN` | 10 | GPIO for SPI DIN |
| `JETTYD_DISPLAY_PIN_CLK` | 8 | GPIO for SPI CLK |
| `JETTYD_DISPLAY_PIN_CS` | 9 | GPIO for SPI CS (LOAD) |
| `JETTYD_DISPLAY_MODULES` | 4 | Daisy-chained module count (1–4) |
| `JETTYD_DISPLAY_BRIGHTNESS` | 3 | Startup intensity (0 min – 15 max) |

## `display.set` command

**Topic:** `<tenant>/<device>/command`

**Payload:**
```json
{
  "id": "cmd_abc123",
  "action": "display.set",
  "params": {
    "value": 123456,
    "brightness": 5
  }
}
```

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `value` | string \| number | yes | Text or number to display |
| `brightness` | integer 0–15 | no | Update intensity; omit to keep current |

**Response:**
```json
{"id": "cmd_abc123", "status": "acked", "result": {}}
```

### Validation rules

- `value` missing, `null`, boolean, or object → `status: rejected`
- `brightness` outside 0–15 → `status: rejected`; no display or state change

## Number compaction

Numeric values are automatically compacted to fit the ~5-character display:

| Range | Example input | Rendered |
|-------|--------------|---------|
| 0 – 99 999 | 42 | `42` |
| 100 000 – 999 999 | 150000 | `150k` |
| 1 000 000 – 9 999 999 | 1500000 | `1.5M` |
| 10 000 000 – 999 999 999 | 99000000 | `99M` |
| ≥ 1 000 000 000 | 2000000000 | `2B` |
| negative | -1 | `----` |

## String values

String values are rendered as-is, truncated to 5 characters. Supported
characters: `0–9`, `k`, `M`, `B`, `.`, `-`, ` `. Unsupported characters
render as blank.

## Sending the command via the platform API

```bash
curl -X POST https://api.jettyd.com/v1/devices/<device-id>/commands \
  -H "Authorization: Bearer <api-key>" \
  -H "Content-Type: application/json" \
  -d '{
    "command_type": "display.set",
    "payload": { "value": 42000, "brightness": 8 }
  }'
```

## Font

The built-in 5×8 ASCII font supports digits `0–9` and the compaction suffixes
`k`, `M`, `B`, `.`, `-`. Characters outside this set render as blank columns.

## Implementation notes

- **No Arduino libraries.** Native ESP-IDF bit-bang SPI over three GPIO pins.
- **FC16 layout.** Module 0 = leftmost visual position; module index increases
  left-to-right. In the SPI chain, module 0 is the last chip (receives the
  final 16 bits before CS/LOAD goes high).
- **Atomic update.** Brightness and frame buffer are both applied before
  returning `acked`.
- **No scrolling.** Static centred render only.
