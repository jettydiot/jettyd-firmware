#!/bin/bash
# hil-smoke.sh — hardware-in-the-loop smoke gate for jettyd firmware.
#
# Builds the firmware, flashes it to the connected test device, and watches
# the serial console for boot markers. This is the merge gate for any change
# to the firmware SDK or template: no firmware PR may be opened without a
# PASS from this script (paste the verdict block into the PR body).
#
# Usage:
#   tools/hil-smoke.sh [project_dir]     # default: ../jettyd-firmware-template
#   tools/hil-smoke.sh --restore         # flash latest known-good binary
#
# Exit codes: 0 = PASS, 1 = FAIL, 2 = device/lock/toolchain unavailable.
set -uo pipefail

PORT="${JETTYD_HIL_PORT:-/dev/cu.usbmodem1101}"
IDF="${IDF_PATH:-$HOME/esp/esp-idf}"
SDK_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PROJECT_DIR="${1:-$SDK_DIR/../jettyd-firmware-template}"
LOCK_DIR="$HOME/.jettyd/hil.lock"
KNOWN_GOOD="$HOME/.jettyd/known-good"
MONITOR_SECS="${JETTYD_HIL_MONITOR_SECS:-45}"
LOG="$(mktemp /tmp/hil-smoke.XXXXXX)"

fail() { echo "HIL_SMOKE: FAIL — $1"; echo "log: $LOG"; release_lock; exit "${2:-1}"; }

acquire_lock() {
  mkdir -p "$(dirname "$LOCK_DIR")"
  for i in $(seq 1 60); do
    if mkdir "$LOCK_DIR" 2>/dev/null; then
      echo $$ > "$LOCK_DIR/pid"; return 0
    fi
    # stale lock: holder dead, or lock older than 30 min
    holder=$(cat "$LOCK_DIR/pid" 2>/dev/null || echo "")
    if [ -n "$holder" ] && ! kill -0 "$holder" 2>/dev/null; then
      rm -rf "$LOCK_DIR"; continue
    fi
    if [ -n "$(find "$LOCK_DIR" -maxdepth 0 -mmin +30 2>/dev/null)" ]; then
      rm -rf "$LOCK_DIR"; continue
    fi
    sleep 10
  done
  echo "HIL_SMOKE: FAIL — could not acquire device lock after 10 min (held by pid $holder)"; exit 2
}
release_lock() { rm -rf "$LOCK_DIR" 2>/dev/null || true; }
trap release_lock EXIT

[ -e "$PORT" ] || { echo "HIL_SMOKE: FAIL — no device on $PORT. Escalate: hardware unplugged."; exit 2; }
[ -f "$IDF/export.sh" ] || { echo "HIL_SMOKE: FAIL — ESP-IDF not found at $IDF"; exit 2; }

acquire_lock
# shellcheck disable=SC1091
. "$IDF/export.sh" > /dev/null 2>&1 || fail "ESP-IDF export.sh failed" 2

if [ "${1:-}" = "--restore" ]; then
  BIN=$(ls -t "$KNOWN_GOOD"/*.bin 2>/dev/null | head -1)
  [ -n "$BIN" ] || fail "no known-good binary archived" 2
  # 0x20000 = ota_0 app offset per partitions.csv
  esptool.py --port "$PORT" write_flash 0x20000 "$BIN" >> "$LOG" 2>&1 || fail "restore flash failed"
  echo "HIL_SMOKE: RESTORED $BIN"; exit 0
fi

cd "$PROJECT_DIR" || fail "project dir not found: $PROJECT_DIR" 2

echo "→ Building ($PROJECT_DIR)..."
idf.py build >> "$LOG" 2>&1 || { tail -30 "$LOG"; fail "build failed"; }

echo "→ Flashing $PORT..."
idf.py -p "$PORT" flash >> "$LOG" 2>&1 || { tail -20 "$LOG"; fail "flash failed"; }

echo "→ Monitoring serial for ${MONITOR_SECS}s..."
python3 - "$PORT" "$MONITOR_SECS" >> "$LOG" 2>&1 <<'PYEOF'
import sys, time, serial

port, secs = sys.argv[1], int(sys.argv[2])
CRASH = ("Guru Meditation", "abort()", "Panic", "Brownout", "CORRUPT HEAP",
         "assert failed", "rst:0x8", "invalid header")
BOOT_OK = "Calling app_main()"
SDK_OK = "Driver registry initialized"

ser = None
for _ in range(20):  # port re-enumerates after USB-JTAG reset
    try:
        ser = serial.Serial(port, 115200, timeout=1)
        break
    except (OSError, serial.SerialException):
        time.sleep(1)
if ser is None:
    print("MONITOR_VERDICT: FAIL port never reappeared"); sys.exit(1)

deadline = time.time() + secs
booted = sdk_up = False
resets = 0
while time.time() < deadline:
    line = ser.readline().decode(errors="replace").rstrip()
    if not line:
        continue
    print(line)
    if BOOT_OK in line:
        resets += 1
        booted = True
        if resets >= 3:
            print("MONITOR_VERDICT: FAIL boot loop (%d resets)" % resets); sys.exit(1)
    if SDK_OK in line:
        sdk_up = True
    for c in CRASH:
        if c in line:
            print("MONITOR_VERDICT: FAIL crash marker: %s" % c); sys.exit(1)
    if sdk_up and time.time() > deadline - (secs - 15):
        break  # SDK up and 15s crash-free is enough

if booted and sdk_up:
    print("MONITOR_VERDICT: PASS"); sys.exit(0)
if booted:
    print("MONITOR_VERDICT: FAIL app_main ran but SDK never initialized"); sys.exit(1)
print("MONITOR_VERDICT: FAIL no boot marker seen"); sys.exit(1)
PYEOF
MON=$?

grep -E "MONITOR_VERDICT|Guru|abort|Panic" "$LOG" | tail -5
if [ $MON -ne 0 ]; then fail "smoke monitor failed (full serial log: $LOG)"; fi

# Archive known-good binary for recovery
mkdir -p "$KNOWN_GOOD"
SHA=$(git -C "$PROJECT_DIR" rev-parse --short HEAD 2>/dev/null || echo "nogit")
BIN=$(ls "$PROJECT_DIR"/build/*.bin 2>/dev/null | grep -v bootloader | grep -v partition | head -1)
[ -n "$BIN" ] && cp "$BIN" "$KNOWN_GOOD/$(basename "$PROJECT_DIR")-$SHA-$(date +%Y%m%d%H%M).bin"
ls -t "$KNOWN_GOOD"/*.bin 2>/dev/null | tail -n +6 | xargs rm -f 2>/dev/null

echo "HIL_SMOKE: PASS — chip=$(esptool.py --port "$PORT" chip_id 2>/dev/null | grep -m1 'Chip is' || echo unknown), commit=$SHA, log=$LOG"
exit 0
