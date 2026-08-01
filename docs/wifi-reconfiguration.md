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
provide SoftAP fallback/recovery, BLE provisioning, SmartConfig, or phone-app
onboarding — those are separate features.
