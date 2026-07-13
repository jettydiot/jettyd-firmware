# camera driver

ESP32-S3 + OV2640 JPEG capture with upload via the jettyd media grant flow.

**Hardware requirement:** ESP32-S3 with PSRAM and OV2640 camera module.  
**Target guard:** compiles to a no-op stub on non-camera targets (esp32c3, etc.) — all CI legs stay green.

## device.yaml config

```yaml
drivers:
  - name: camera
    instance: cam
    config:
      sensor: ov2640
      frame_size: svga
      jpeg_quality: 12
      capture_interval_sec: 0
      grant_timeout_sec: 30
```

### Config reference

| Field | Type | Default | Valid values | Description |
|-------|------|---------|-------------|-------------|
| `sensor` | string | — | `ov2640` (required) | Camera sensor model. Only `ov2640` is supported in v1. Codegen rejects other values with a clear error. |
| `frame_size` | string | `svga` | `qvga`, `vga`, `svga`, `xga`, `uxga` | JPEG output resolution (QVGA=320×240 … UXGA=1600×1200). |
| `jpeg_quality` | int | `12` | 0–63 | JPEG compression quality. 0 = highest quality (largest file); 63 = lowest quality (smallest file). Values above 63 are clamped. |
| `capture_interval_sec` | uint32 | `0` | 0–∞ | Automatic periodic capture interval in seconds. `0` = interval disabled; capture only via MCP `camera_capture` tool. |
| `grant_timeout_sec` | uint32 | `30` | 1–∞ | Seconds to wait for `media/grant` before releasing the framebuffer and logging a failure. |

## ESP32-S3-CAM wiring (OV2640 DVP preset)

The driver uses a named pin preset for the standard ESP32-S3-CAM board.
No GPIO numbers appear in runtime logic — all pins are in `s_ov2640_s3cam_preset`
in `camera.c`.

| Signal | GPIO |
|--------|------|
| D0–D7 | 39, 40, 41, 42, 43, 44, 45, 48 |
| XCLK | 10 |
| PCLK | 11 |
| VSYNC | 6 |
| HREF | 7 |
| SCCB SDA | 4 |
| SCCB SCL | 5 |
| PWDN | — (not connected) |
| RESET | — (not connected) |

PSRAM must be enabled in `sdkconfig` (`CONFIG_ESP32S3_SPIRAM_SUPPORT=y`).

## Capture triggers

| Trigger | How |
|---------|-----|
| Interval | Set `capture_interval_sec > 0`; a FreeRTOS timer fires at that interval. |
| MCP command | Call MCP tool `camera_capture` over MQTT (`mcp/call` topic). Non-blocking — returns immediately while upload runs asynchronously. |
| GPIO/button | Deferred to a follow-up ticket (FLU-159). |

## AE/AWB settling

The OV2640's first frames after sensor power-up are underexposed — the very
first frame is typically **pitch black**, and auto-exposure/auto-white-balance
converge only after several frames (~3–5 during P4 camera bring-up). Without
settling, a grant-triggered capture that grabs the first frame would upload a
black/garbage image.

To avoid this, right after `esp_camera_init()` the driver grabs and immediately
**discards** a configurable number of settle frames, so the first frame it
actually uploads is properly exposed. The driver keeps the sensor powered
between captures, so a single settle pass at init is sufficient. (If a future
revision powers the sensor down between captures, settling must be repeated on
each wake before the upload capture.)

Configured at build time via Kconfig (not `device.yaml`):

| Kconfig symbol | Default | Range | Meaning |
|----------------|---------|-------|---------|
| `CONFIG_JETTYD_CAMERA_SETTLE_FRAMES` | `5` | 0–30 | Frames to grab-and-discard after sensor init. `0` disables settling. |

The default of 5 matches the ~3–5 frame convergence observed on hardware. Host
unit tests, which have no `sdkconfig`, fall back to the same default of 5.

## Upload flow

The `media/grant` MQTT callback runs on the esp-mqtt client task, so it must
return quickly. It only parses and validates the grant, then hands the
presigned URL to a **dedicated upload task** via a FreeRTOS queue. That task
performs the blocking, multi-second HTTPS PUT and publishes `media/complete`,
so telemetry, commands, and MQTT keepalive are never stalled by an upload.

State machine: `IDLE → AWAIT_GRANT → UPLOADING → IDLE`. State transitions are
serialised with a `portMUX` critical section (the MQTT task, timer daemon, MCP
dispatch, and upload task all touch it). The grant timeout callback only acts
while state is `AWAIT_GRANT`; once a grant has been accepted (`UPLOADING`) a
late/queued timeout callback is a no-op, so it can never release a framebuffer
the upload task is still streaming.

```text
Device                                 Platform
  │                                       │
  │──── capture JPEG ────────────────────►│
  │                                       │
  │──── media/request ──────────────────►│
  │      {id, size, format}               │
  │                                       │
  │◄─── media/grant ────────────────────│
  │      {url: "https://...presigned..."}  │
  │                                       │
  │──── HTTPS PUT (streamed) ───────────►│ S3 / object store
  │      (sha256 computed during send)    │
  │                                       │
  │──── media/complete ─────────────────►│
  │      {id, sha256, status, size}       │
  │                                       │
```

### Quota exceeded path

```text
  │◄─── media/grant ────────────────────│
  │      {quota_exceeded: true,           │
  │       retry_after_sec: 60}            │
  │                                       │
  │  [backoff: suppress requests          │
  │   for retry_after_sec seconds]        │
```

### Grant timeout path

```text
  │──── media/request ──────────────────►│
  │                                       │
  │  ... grant_timeout_sec elapses ...    │
  │                                       │
  │  [release framebuffer]                │
  │  [increment upload_errors]            │
  │  [shadow update: cam.upload_errors]   │
```

## Shadow reporting

The driver reports `{instance}.upload_errors` (integer) to the device shadow
after every grant timeout or PUT failure. Example key: `cam.upload_errors`.

## BLOCKER note (from FLU-157)

The HIL rig board is an ESP32-C3 (no camera interface). The smoke gate verifies
non-camera targets still boot correctly. Full capture verification (actual JPEG
upload round-trip) requires an ESP32-S3 + OV2640 board on the rig — flag in PR
if only C3 HIL evidence is available.

End-to-end upload also depends on FLU-153 (platform grant flow) being deployed.
