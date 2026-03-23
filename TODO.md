# Firmware SDK — Platform Integration TODOs

## Missing Platform Endpoint

The firmware spec (section 7) requires a `PUT /devices/{device_id}/config` endpoint for pushing JettyScript rules to devices. **This endpoint does not currently exist in the platform API.**

The platform needs:

```
PUT /v1/devices/{device_id}/config
Authorization: Bearer tk_xxx

{
  "version": 1,
  "rules": [ ... ],
  "heartbeats": [ ... ]
}
```

This endpoint should:
1. Validate the config against the device type's capability schema
2. Store the config in the device record (new `config` column or JSON field)
3. Publish to MQTT topic `jettyd/{tenant_id}/{device_id}/config`
4. Return 202 Accepted (async delivery to device)
5. On device reconnect, push the stored config via retained message or on-connect push

**Files to modify:** `apps/platform/src/routes/devices.rs` (add config endpoint), `apps/platform/src/models/mod.rs` (add config field to Device model)

## P1/P2 Drivers Not Yet Implemented

The following drivers from the spec are not yet implemented:
- P1: bme280, pwm_output, hcsr04, ina219
- P2: bh1750, gps_nmea, hx711, mq_gas, solenoid_valve, servo, buzzer
- P3: modbus_rtu, ble_scanner

## Other TODOs

- [ ] `tools/flash.py` — Flash + provision helper (writes NVS partition)
- [ ] `tools/simulate.py` — Software simulator for CI integration testing
- [ ] NVS persistence in vm.c needs full round-trip serialization (currently simplified)
- [ ] Deep sleep support in jettyd.c main loop
- [ ] Battery ADC reading in telemetry.c (currently returns 0)
- [ ] SNTP time sync for schedule conditions in VM
- [ ] Rate limiting on telemetry publishing to prevent MQTT flooding
- [ ] Watchdog registration for the VM task
