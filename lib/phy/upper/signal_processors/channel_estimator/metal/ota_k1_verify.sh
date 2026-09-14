#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
#
# OTA (over-the-air) leg for the K1-on-GPU verification.
#
#   ota_k1_verify.sh [seconds] [config]      run the leg, then report
#   ota_k1_verify.sh report <console> <log>  report on an existing pair of logs
#
# It runs the full Metal UL chain on the air for a fixed window and SIGINTs the gNB so the
# exit-time counters print, then judges the acceptance criteria of the K1 device-inversion work
# (S-7f-4f).
#
# Run it once a UE is attached and driving PUSCH traffic - a leg with no UE exercises no channel
# estimator at all, so every CE counter stays at zero and nothing is proven. The gNB also exits on
# its own when the AMF is down ("CU-CP failed to connect to AMF"), which this reports instead of
# waiting out the window.
#
# WHERE THE EVIDENCE IS: the [metal_stats] / [mmse_time_sum] counters are written to STDERR by
# std::atexit handlers, so they appear in the CONSOLE log and only on a CLEAN exit - a SIGKILL
# loses them (hence the long grace period below). The application log carries the link events
# (Real-time failure in RF, UE/RRC/NGAP activity). Both are needed.
set -u

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../../../.." && pwd)
CFG_DEFAULT=configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml

# ---------------------------------------------------------------- report -----
report() {
  local CONSOLE=$1 LOG=$2
  # Never slurp the logs into a variable: an OTA application log runs to hundreds of MB.
  local FILES=()
  [ -f "$CONSOLE" ] && FILES+=("$CONSOLE")
  [ -f "$LOG" ] && FILES+=("$LOG")
  [ "${#FILES[@]}" -gt 0 ] || { echo "no logs at $CONSOLE / $LOG"; return 2; }
  # -h: no filenames (the report is about the leg, not about which sink held a line).
  g() { grep -h "$@" "${FILES[@]}" 2>/dev/null; }
  gc() { grep -hc "$@" "${FILES[@]}" 2>/dev/null | awk '{s+=$1} END{print s+0}'; }
  gci() { grep -hci "$@" "${FILES[@]}" 2>/dev/null | awk '{s+=$1} END{print s+0}'; }

  echo "== provenance =="
  g -m1 "Built in" || echo "   (no build stamp found)"

  if g -q "failed to connect to AMF"; then
    echo "== ABORTED: the AMF (NGAP) is not reachable - start the core, then re-run =="
    g -m1 "NG Setup"
    return 1
  fi

  echo
  echo "== 1. the device path really executed (console log; stderr atexit counters) =="
  local MMSE
  MMSE=$(grep -ho "mmse_ce commits=[0-9]* waits=[0-9]* max_in_flight=[0-9]*.*" "$CONSOLE" 2>/dev/null | tail -1)
  if [ -n "$MMSE" ]; then
    echo "   $MMSE"
    local dc cf
    dc=$(sed -E 's/.*device_corr_builds=([0-9]+).*/\1/' <<<"$MMSE")
    cf=$(sed -E 's/.*corr_build_fail=([0-9]+).*/\1/' <<<"$MMSE")
    echo "   => device_corr_builds=$dc corr_build_fail=$cf"
  else
    echo "   MISSING - no [metal_stats] mmse_ce line. Either the CE never ran, or the process was"
    echo "   SIGKILLed before its atexit handlers could print (see the runner's warning)."
  fi
  local TF
  TF=$(grep -ho "\[mmse_time_sum\].*" "$CONSOLE" 2>/dev/null | tail -1)
  [ -n "$TF" ] && echo "   $TF" || echo "   (no [mmse_time_sum] line)"

  echo
  echo "== 2. RF / link health (recorded, NOT gated beyond a sanity bound) =="
  local rt_total rt_under rt_late rt_other
  rt_total=$(gc "Real-time failure in RF")
  rt_under=$(gc "Real-time failure in RF: underflow")
  rt_late=$(gc "Real-time failure in RF: late")
  rt_other=$(( rt_total - rt_under - rt_late ))
  echo "   Real-time failure in RF: total=$rt_total (underflow=$rt_under late=$rt_late other=$rt_other)"
  echo -n "   USB error lines: "
  gci -E "usb.*(error|fail|overflow|underflow)"

  echo
  echo "== 3. crashes =="
  echo -n "   crash-signature lines: "
  gci -E "assert|terminate|segmentation|core dumped"
  g -E "assert|terminate|segmentation|core dumped" | tail -3 || true

  echo
  echo "== 4. a UE was actually there (a leg with no UE proves nothing) =="
  echo -n "   RRC/NGAP/CU/GTPU/DU-MNG app-log lines: "
  gci -E "\[(RRC|NGAP|CU-CP|CU-UP|GTPU|DU-MNG)[[:space:]]*\]"
  echo -n "   PRACH / RNTI / PUSCH mentions: "
  gci -E "prach|rnti|pusch"

  echo
  echo "== raw logs =="
  echo "   console (counters): $CONSOLE"
  echo "   app log (link)    : $LOG"
}

# -------------------------------------------------------------------- run -----
if [ "${1:-}" = report ]; then
  report "${2:?usage: $0 report <console> <log>}" "${3:?usage: $0 report <console> <log>}"
  exit $?
fi

DUR=${1:-150}
CFG=${2:-$CFG_DEFAULT}
# Timestamped sinks: a fixed name is a trap. An earlier version of this script removed the fixed
# paths before starting, which UNLINKED the log a concurrently running gNB still had open - its
# counters kept going to a deleted inode and were unrecoverable.
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
  echo "stop it first (SIGINT, so its atexit counters print), then re-run this script."
  exit 3
fi

echo "console (counters): $CONSOLE"
echo "app log (link)    : $LOG"
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
if kill -0 $PID 2>/dev/null; then
  # SIGINT, never SIGTERM/SIGKILL: the [metal_stats]/[mmse_time_sum] counters are std::atexit
  # handlers and only run on a clean shutdown.
  kill -INT $PID 2>/dev/null
  for _ in $(seq 1 60); do
    kill -0 $PID 2>/dev/null || break
    sleep 1
  done
  if kill -0 $PID 2>/dev/null; then
    echo "WARNING: the gNB did not exit within 60s of SIGINT; SIGKILLing it - THE ATEXIT COUNTERS"
    echo "         WILL BE LOST. The link-health section below is still valid."
    kill -9 $PID 2>/dev/null
  fi
fi
wait $PID 2>/dev/null
echo

report "$CONSOLE" "$LOG"
