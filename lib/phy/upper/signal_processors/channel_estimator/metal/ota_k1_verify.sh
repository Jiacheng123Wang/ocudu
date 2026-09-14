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
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../../../.." && pwd)
# Timestamped sinks: a fixed name is a trap. An earlier version of this script removed the fixed
# paths before starting, which UNLINKED the log a concurrently running gNB still had open - its
# counters kept going to a deleted inode and were unrecoverable. Unique names make that impossible.
STAMP=$(date +%Y%m%d-%H%M%S)
LOG=/tmp/gnb_ota_k1_$STAMP.log
CONSOLE=/tmp/gnb_ota_k1_${STAMP}_console.log

cd "$REPO" || exit 1

# Never race another gNB: the NG-U gateway binds a fixed UDP port, so a second instance dies at
# startup ("Failed to bind UDP socket ... Address already in use") and, worse, the operator's
# running leg keeps writing to sinks this script would otherwise touch.
if pgrep -f "build/apps/gnb/gnb" >/dev/null 2>&1; then
  echo "a gNB is already running:"
  pgrep -fl "build/apps/gnb/gnb"
  echo "stop it first (its own counters print on SIGINT), then re-run this script."
  exit 3
fi

echo "logs: $CONSOLE (counters)  $LOG (link events)"
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

# The counters go to stderr (the console), the application log holds the link events. Some builds
# route both to the console, so the greps below look at both files.
BOTH=$(cat "$CONSOLE" "$LOG" 2>/dev/null)

echo "== binary =="
printf '%s\n' "$BOTH" | grep -m1 "Built in" || echo "   (no log at all - did the binary start?)"

if printf '%s\n' "$BOTH" | grep -q "failed to connect to AMF"; then
  echo "== ABORTED: the AMF (NGAP) is not reachable - start the core, then re-run =="
  printf '%s\n' "$BOTH" | grep -m1 "NG Setup"
  exit 1
fi

echo "== device path really executed =="
echo "   device_corr_builds > 0 and hops_gpu > 0 prove the estimator ran; the K1 counters below"
echo "   prove the device inversion ran (commits grow by one per batch)."
printf '%s\n' "$BOTH" | grep -o "\[metal_stats\] mmse_ce commits=.*" | tail -1
printf '%s\n' "$BOTH" | grep -o "\[mmse_time_sum\].*" | tail -1

echo "== per-hop timing (recorded, NOT gated: the K1 kernel is a known performance debt) =="
printf '%s\n' "$BOTH" | grep -o "\[ul_time_frequency\].*" | tail -1

echo "== link health =="
echo -n "   Real-time failure in RF lines: "
printf '%s\n' "$BOTH" | grep -ciE "Real-time failure in RF"
printf '%s\n' "$BOTH" | tail -40 | grep -iE "Real-time failure in RF" || true
echo -n "   USB error lines: "
printf '%s\n' "$BOTH" | grep -ciE "usb.*(error|fail|overflow|underflow)"

echo "== crashes =="
printf '%s\n' "$BOTH" | grep -iE "assert|terminate|segmentation|core dumped" | tail -5 || true

echo "== UE activity (a leg with no UE proves nothing) =="
echo -n "   PRACH / RNTI lines: "
printf '%s\n' "$BOTH" | grep -cE "PRACH|rnti="

echo "== raw outputs kept for the record =="
echo "   $CONSOLE"
echo "   $LOG"
