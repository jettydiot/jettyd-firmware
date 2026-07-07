# jettyd-firmware SDK — Agent Guide

ESP-IDF (v5.3.2 at `~/esp/esp-idf`) component SDK for jettyd devices. Host unit
tests live in `test/` (mocked, no hardware). The sibling repo
`~/Projects/jettyd-firmware-template` is the reference integration target.

## Dev commands

- `make test` — host unit tests (fast, always run first)
- `make check` — unit tests + IDF build against the template
- `tools/hil-smoke.sh` — hardware-in-the-loop gate (see below)
- `tools/hil-smoke.sh --restore` — reflash latest known-good binary if the device is wedged

## Hardware-in-the-loop gate (MANDATORY before any PR)

A physical ESP32-C3 test device is connected to this machine on
`/dev/cu.usbmodem1101` (override: `JETTYD_HIL_PORT`). Agents are authorized to
build, flash, and test on it autonomously (Tom, 2026-07-07).

Rules:

1. **Every firmware PR needs a PASS.** Run `tools/hil-smoke.sh` after your
   change and paste the `HIL_SMOKE: PASS` verdict line (with commit sha) into
   the PR body under a `## HIL Evidence` heading. Reviewers reject PRs without it.
2. **One device, one user.** The script takes an exclusive lock
   (`~/.jettyd/hil.lock`). Never flash or open the serial port outside the
   script; never delete the lock of a live process.
3. **Exit code 2 = escalate, don't retry.** It means no device / no toolchain /
   lock starvation. Post to the escalation channel; do not loop.
4. **Wedged device:** run `tools/hil-smoke.sh --restore` once. If the device
   still doesn't enumerate, escalate — that needs a human hand on the cable.
5. **Feature-specific validation:** the smoke gate proves boot + SDK init. If
   your ticket adds device-visible behavior (new MQTT topic, tool dispatch,
   driver), extend the evidence: flash, then show the relevant serial log lines
   or MQTT round-trip proving the feature works on-device. "Builds and boots"
   alone does not validate a feature.

## Do-Not-Touch (escalate instead of editing)

- `jettyd/src/ota.c` signing/rollback logic and `partitions.csv` layouts in
  consuming projects — a bad change here can strand remote devices. Additive
  changes require Tom's review; the PR must be labeled and NOT auto-merged.

## Merge policy

- Host tests green + `make check` green + HIL PASS + no OTA/partition/signing
  changes → PR may be auto-merged per the standard pipeline policy.
- OTA, partition table, flash layout, or crypto/signing changes → always hold
  for Tom.

## Quality Gates

- test: `make test`
- lint: `make check`   # host tests + full IDF build against the template — the real compile truth
- typecheck: `true`    # C: covered by the IDF build in `make check`

Run `make test` and `make check` before every commit (host stubs miss real
errors — the IDF build is the truth). POSIX feature macros
(`_POSIX_C_SOURCE`) go in Makefile CFLAGS, never headers. The HIL gate
(`tools/hil-smoke.sh`) is additionally mandatory before opening a PR — see above.
