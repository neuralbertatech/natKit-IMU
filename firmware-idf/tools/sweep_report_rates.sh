#!/usr/bin/env bash
# Sweep BNO08x per-report intervals and record what the hub actually delivers.
#
# ⚠️ THIS MEASURES THE SENSOR, NOT THE LINK. The rates come off the leaf's own
# console, so nothing here depends on ESP-NOW, the primary, or the broker -- which
# matters, because a node can be delivering nothing over the air while its reports
# are perfectly healthy.
#
# Reads: 100 Hz is a FLOOR, not a target. The frame builder takes a 100 Hz
# snapshot of each sensor's latest value, so a report above 100 Hz is merely
# wasted while one below leaves stale values in samples.
#
# usage: sweep_report_rates.sh <port>   (from firmware-idf/)

set -uo pipefail
PORT="${1:-/dev/ttyACM0}"
CFG=build/esp32-leaf/sdkconfig
MON="$HOME/natkit-verification/monitor.py"

# accel:gyro:mag:quat intervals in us, 0 = report disabled. Each line is one build.
CASES=(
  "10000:9000:9000:0|drop rotation, gyro+mag asked 111 Hz"
  "10000:9000:5000:0|drop rotation, mag asked 200 Hz"
  "10000:9000:10000:10000|all four, only gyro asked fast"
  "14000:9000:10000:9000|all four, accel throttled hard"
)

set_iv() { sed -i "s/^CONFIG_NATKIT_IMU_INTERVAL_$1_US=.*/CONFIG_NATKIT_IMU_INTERVAL_$1_US=$2/" "$CFG"; }

printf '%-46s %s\n' "CONFIG" "DELIVERED (accel / gyro / mag / quat)"
for case in "${CASES[@]}"; do
  ivs="${case%%|*}"; label="${case##*|}"
  IFS=: read -r a g m q <<<"$ivs"
  set_iv ACCEL "$a"; set_iv GYRO "$g"; set_iv MAG "$m"; set_iv QUAT "$q"

  ./build-role.sh leaf esp32 >/dev/null 2>&1 || { echo "build failed: $label"; continue; }
  ./build-role.sh leaf esp32 -p "$PORT" flash >/dev/null 2>&1 || { echo "flash failed: $label"; continue; }

  # Skip the first ~15 s: the boot sweep and the hub's own settling are not the
  # steady state this is trying to measure.
  line=$(timeout 60 python3 "$MON" "$PORT" 55 2>/dev/null | grep "reports:" | tail -3 | head -1)
  rates=$(sed -E 's/.*reports: //; s/ +\(asked.*//' <<<"$line")
  printf '%-46s %s\n' "$label" "${rates:-NO DATA}"
done
