#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
#
# OTA (over-the-air) leg for the K1-on-GPU verification: run the full Metal UL chain on the air for a
# fixed window, then SIGINT the gNB so the exit-time counters print, and report the acceptance
# criteria of the K1 device-inversion work (S-7f-4f).
#
# Run it once a UE is attached and driving PUSCH traffic - a leg with no UE exercises no channel
# estimator at all, so every CE counter stays at zero and nothing is proven. The gNB also exits on
# its own if the AMF is down ("CU-CP failed to connect to AMF"), which this script reports instead
# of waiting out the window.
#
#   lib/phy/upper/signal_processors/channel_estimator/metal/ota_k1_verify.sh [seconds] [config]
#
# Defaults: 150 s, configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml. sudo is not required to open the B200
# on macOS; add it in front if the local UHD setup needs it.
set -u

DUR=${1:-150}
CFG=${2:-configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml}
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../../.." && pwd)
LOG=/tmp/gnb_ota_k1.log
CONSOLE=/tmp/gnb_ota_k1_console.log

cd "$REPO" || exit 1
rm -f "$LOG" "$CONSOLE"

./build/apps/gnb/gnb -c "$CFG" \
  --expert_phy.pusch_channel_estimator_algo metal_mmse \
  --expert_phy.pusch_channel_equalizer_backend metal \
  --expert_phy.pusch_dft_type metal \
  --expert_phy.pusch_ldpc_decoder_type auto \
  --log.all_level warning --log.filename "$LOG" \
  > "$CONSOLE" 2>&1 &
PID=$!

# Stop early when the gNB aborts by itself (an AMF that is not up ends the run in a few seconds).
for _ in $(seq 1 "$DUR"); do
  kill -0 $PID 2>/dev/null || break
  sleep 1
done
kill -INT $PID 2>/dev/null
for _ in $(seq 1 30); do
  kill -0 $PID 2>/dev/null || break
  sleep 1
done
kill -9 $PID 2>/dev/null
wait $PID 2>/dev/null

echo "== binary =="
grep -m1 "Built in" "$LOG" || true

if grep -q "failed to connect to AMF" "$LOG"; then
  echo "== ABORTED: the AMF (NGAP) is not reachable - start the core, then re-run =="
  grep -m1 "NG Setup" "$LOG"
  exit 1
fi

echo "== device build really executed =="
echo "   (device_corr_builds > 0 and hops_gpu > 0 prove the CE ran on the device path)"
grep -o "\[metal_stats\] mmse_ce commits=[^\"]*" "$LOG" | tail -1

echo "== per-hop timing (recorded, NOT gated: the K1 kernel is a known performance debt) =="
grep -o "\[mmse_time_sum\][^\"]*" "$LOG" | tail -1

echo "== link health =="
grep -ciE "Real-time failure in RF" "$LOG" | sed 's/^/   Real-time failure in RF lines: /'
grep -iE "Real-time failure in RF" "$LOG" | tail -3
grep -ciE "usb|libusb|overflow|underflow" "$LOG" | sed 's/^/   USB/overflow lines: /'

echo "== crashes =="
grep -iE "assert|terminate|segmentation|abort" "$LOG" | tail -5

echo "== ACLR / UE activity (a leg with no UE proves nothing) =="
grep -cE "PRACH|rnti=" "$LOG" | sed 's/^/   PRACH+RNTI lines: /'
