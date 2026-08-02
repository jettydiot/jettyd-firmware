# WiFi Reconfiguration — `wifi.set` Device Command

FLU-163 adds a `wifi.set` device command that updates the device's WiFi
credentials in NVS at runtime and reconnects to the new network — with
**automatic rollback** to the previous network if the switch fails. This lets
you move a deployed device onto a new SSID from the dashboard or an agent
without reflashing.

## Command

`wifi.set` arrives on the standard command topic
`jettyd/{tenant_id}/{device_key}/command` and follows the standard
`command_type` / `payload` shape:

```json
{
  "id": "cmd_abc123",
  "command_type": "wifi.set",
  "payload": {
    "ssid": "NewNetwork",
    "password": "s3cret-pass"
  }
}
```

Fields:

| Field | Required | Notes |
|-------|----------|-------|
| `payload.ssid` | yes | 1–32 bytes. |
| `payload.password` | no | 0–64 bytes. Empty or absent = **open network**. |

The `action` / `params` spelling is accepted as an equivalent alias
(`"action": "wifi.set"`, `"params": { … }`), matching the other device
commands.

### Payload validation

The payload is validated **before** any NVS write. An invalid payload is
rejected with no change to stored credentials and no reconnect:

- SSID empty or longer than 32 bytes → `rejected`
- password longer than 64 bytes → `rejected`

## Result

The device replies on `jettyd/{tenant_id}/{device_key}/command/response`,
using the same `{ "id", "status", "result" }` shape as every other command
(e.g. `servo.rotate`):

Success (connected on the new network):

```json
{
  "id": "cmd_abc123",
  "status": "acked",
  "result": { "wifi.ssid": "NewNetwork", "connected": true }
}
```

Rejected (invalid payload):

```json
{
  "id": "cmd_abc123",
  "status": "rejected",
  "result": { "error": "ssid exceeds 32 bytes" }
}
```

Failed (new network unreachable — rolled back to the previous one):

```json
{
  "id": "cmd_abc123",
  "status": "failed",
  "result": { "error": "could not join new network; rolled back to previous WiFi" }
}
```

## Rollback contract

On a valid payload the device:

1. Holds the current SSID/password in RAM.
2. Writes the new values to the existing NVS keys `wifi_ssid` / `wifi_pass`
   in namespace `jettyd_prov`. The provisioning identity keys — `device_key`,
   `fleet_token`, `tenant_id` — are **never** touched.
3. Disconnects and reconnects to the new network.
4. If the new network reaches a connected state within the timeout, the switch
   succeeds and the new credentials stay in NVS.
5. If it does **not** connect within the timeout, the device restores the exact
   previous credentials to NVS, reconnects to the previous network, and reports
   `failed`.

An empty password stores an empty string (open network); a later `wifi.set`
back to a WPA2 network restores the password normally.

## Ack timing caveat

Switching networks necessarily drops the MQTT session — the device cannot
publish while it is between networks. Because of that:

- The **success** ack is published only after the MQTT session has
  re-established on the **new** network.
- The **failure** ack is published after MQTT reconnects on the **previous**
  network following rollback.

In both cases the response is buffered across the switch and flushed on
reconnect, so expect a delay between sending `wifi.set` and seeing the result —
up to roughly the reconnect timeout plus MQTT re-establishment time. A missing
ack within that window means the switch (or rollback) is still in progress, not
that the command was lost.

## Timeout configuration

The time the device waits for the new network before rolling back is a Kconfig
option:

```
CONFIG_JETTYD_WIFI_SET_TIMEOUT_MS   # default 30000, range 5000–120000
```

Set it in `sdkconfig.defaults` or via `idf.py menuconfig`
(*jettyd configuration → wifi.set reconnect timeout (ms)*).

## Sending the command

### From the dashboard

Use the generic "send command" action with `command_type` `wifi.set` and the
`ssid` / `password` payload shown above. No dashboard changes are required —
generic command sending already exists.

### With `mosquitto_pub`

Publish to the device's command topic (substitute your tenant ID and device
key):

```bash
mosquitto_pub \
  -h mqtt.jettyd.com -p 8883 --cafile ca.pem \
  -u "$DEVICE_ID" -P "$DEVICE_KEY" \
  -t "jettyd/$TENANT_ID/$DEVICE_KEY/command" \
  -m '{"id":"cmd_1","command_type":"wifi.set","payload":{"ssid":"NewNetwork","password":"s3cret-pass"}}'
```

Subscribe to `jettyd/$TENANT_ID/$DEVICE_KEY/command/response` to see the ack
once the device is back online.

## Scope

`wifi.set` covers moving between infrastructure networks. It does **not**
provide BLE provisioning, SmartConfig, or phone-app onboarding — those are
separate features. For credential recovery when the device cannot reach any
network at all, see the SoftAP fallback portal below.

---

# SoftAP Fallback Config Portal

FLU-164 adds a SoftAP config portal that activates when station WiFi cannot
connect after a configurable number of consecutive attempts **and** the portal
is armed. From a phone or laptop the user connects to the device's open AP,
opens `http://192.168.4.1`, picks a network, and saves new credentials. The
device writes them to NVS and reboots into the station loop.

## Kconfig options

| Option | Default | Description |
|--------|---------|-------------|
| `CONFIG_JETTYD_WIFI_PORTAL` | `y` | Enable the portal. Set to `n` to compile out all portal code and data. |
| `CONFIG_JETTYD_WIFI_PORTAL_FAIL_THRESHOLD` | `5` | Consecutive station failures before the portal is considered (1–20). |
| `CONFIG_JETTYD_WIFI_PORTAL_BOOT_WINDOW_S` | `300` | Seconds after power-on during which the portal is armed (30–3600). |
| `CONFIG_JETTYD_WIFI_PORTAL_TIMEOUT_S` | `600` | Idle HTTP timeout before the portal reboots back to station retry (60–3600). |

Configure via `idf.py menuconfig` → *jettyd configuration*, or set in
`sdkconfig.defaults`.

## Arming

The portal starts **only** when two conditions hold simultaneously:

1. **Failure threshold reached** — the device has accumulated
   `CONFIG_JETTYD_WIFI_PORTAL_FAIL_THRESHOLD` consecutive station connection
   failures since the last successful IP assignment.

2. **Portal is armed** — at least one of:
   - **(a) Fresh device** — no SSID is stored in NVS (first boot, factory reset).
   - **(b) Boot window** — fewer than `CONFIG_JETTYD_WIFI_PORTAL_BOOT_WINDOW_S`
     seconds have elapsed since power-on. The default 300 s (5 min) window lets a
     user power-cycle a device and immediately reconfigure it.
   - **(c) Boot button held** — GPIO0 is held low at the time of portal evaluation
     (i.e. the user holds the boot/user button through the failed connect attempts).

**Security rationale:** a WiFi jammer driving repeated disconnects cannot summon
the config AP on a long-running unattended device — the device must be within
its boot window or have its button physically held. Only fresh/recently powered-on
devices are vulnerable without physical access.

## AP details

| Property | Value |
|----------|-------|
| SSID | `jettyd-<last 6 chars of device_id>` (e.g. `jettyd-abc123`). Unprovisioned devices use `jettyd-000000`. |
| Security | Open (no WPA2). Connect without a password. |
| IP | `192.168.4.1` (DHCP assigned by the device). |
| Max connections | 4 simultaneous clients. |

## Using the portal

1. On your phone or laptop, connect to the `jettyd-<id>` open WiFi network.
2. Open a browser and navigate to **`http://192.168.4.1`**.
   (There is no captive-portal redirect; you must type the IP manually.)
3. The page shows visible networks from a pre-switch scan. Tap a network name
   to populate the SSID field, enter the password, and click **Save & Reboot**.
4. The device writes the new credentials to NVS and reboots. The AP disappears.
   After the reboot the device connects to the new network and resumes normal
   operation (MQTT, provisioning, etc.).

## HTTP endpoints

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/` | Embedded HTML page — network picker + SSID/password form. |
| `GET` | `/scan` | JSON array of networks cached before the AP switch: `[{"ssid":"…","rssi":-65,"auth":3},…]`. |
| `POST` | `/save` | URL-encoded form body (`ssid=X&password=Y`). Validates (SSID 1–32 bytes, password 0–64 bytes), writes `wifi_ssid` / `wifi_pass` in namespace `jettyd_prov`, returns a confirmation page, and reboots after ~2 s. |

## NVS key contract

`POST /save` writes **only** `wifi_ssid` and `wifi_pass` in the `jettyd_prov`
namespace — identical to the `wifi.set` command's contract. The identity keys
(`device_key`, `fleet_token`, `tenant_id`) are never touched.

## Idle timeout

If no HTTP request arrives within `CONFIG_JETTYD_WIFI_PORTAL_TIMEOUT_S` seconds
(default 600 s / 10 min), the portal closes and the device reboots into the
station retry loop. The stored credentials (if unchanged) are retried again.

## Disabling the portal

Set `CONFIG_JETTYD_WIFI_PORTAL=n` in `sdkconfig.defaults` or via menuconfig.
When disabled, `wifi.c` retries station connection indefinitely (original
behaviour), and no portal code or data is compiled into the firmware image.
