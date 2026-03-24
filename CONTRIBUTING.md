# Contributing to jettyd-firmware

Thanks for taking the time to contribute. This SDK runs on embedded hardware — reliability and safety are non-negotiable.

## Before you start

- Open an issue to discuss significant changes before writing code
- For new drivers, check the existing drivers in `drivers/` for conventions
- The safety review tool (`tools/safety-review.js`) must pass before any PR is merged

## Development setup

1. Install ESP-IDF 5.x: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/
2. Install Node.js 18+ (for the safety review tool)
3. Clone the repo and build a device target:

```bash
cd devices/env-monitor-v1
idf.py build
```

## Safety requirements

All C code must pass the firmware safety review:

```bash
node tools/safety-review.js core/src/mqtt.c
```

Rules enforced:
- No `volatile` violations on shared state
- No blocking calls (`vTaskDelay`, mutex take) inside event handlers
- No dynamic allocation (`malloc`, `cJSON_Print*`) after init in task loops
- Stack usage must not exceed 512 bytes for local arrays in event handlers
- Actuator drivers must implement `max_on_duration` with auto-off timer

## Adding a driver

1. Create `drivers/<name>/<name>.c` and `drivers/<name>/<name>.h`
2. Implement the `jettyd_driver_t` interface (see `core/include/jettyd_driver.h`)
3. Add unit tests in `test/test_drivers.c`
4. Run the safety review on your driver
5. Add a device example in `devices/` if appropriate

## Commit style

```
<type>(<scope>): <short description>

Types: feat, fix, docs, test, refactor, ci
Scope: core, driver/<name>, devices/<name>, tools
```

## Licence

By contributing, you agree your contributions are licensed under MIT.
