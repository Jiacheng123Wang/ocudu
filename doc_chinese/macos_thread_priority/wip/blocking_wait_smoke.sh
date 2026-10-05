#!/usr/bin/env bash
# Offline smoke test of OCUDU_PHY_BLOCKING_WAIT (condition-variable wait for the lower-PHY workers) and of the
# hand-off probe, on the LOOPBACK bench - no radio, no sudo, no UE.
#
# WHY THIS RUNS BEFORE ANY AIR LEG (dev doc 11.49). The change replaces the wait an idle PHY thread uses, so it
# touches two things a leg cannot survive losing:
#   1. SHUTDOWN. A worker parked on a condition variable is woken by request_stop(); if it were not, the thread
#      would never join, the process would ignore TERM, and the leg would end WITHOUT its report - the exact way
#      p14-conc2 lost its readings. That failure mode is invisible on air until it has already cost a leg, and it
#      is trivially visible here.
#   2. THE READING ITSELF. The probe has to produce samples on the path it instruments (rx -> ul), otherwise the
#      air leg would fly a knobs-on run that reports nothing and the whole pair would be void.
# The loopback config uses ru_sdr/device_driver=realtime_loopback, so the lower-PHY workers and the rx->ul hand-off
# are the real ones, on a real slot clock - which is what makes this a verification rather than a rehearsal.
#
# usage:  bash blocking_wait_smoke.sh [seconds-per-run] [runs-per-arm]
set -u

SECS="${1:-15}"
RUNS="${2:-2}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
CFG="doc_chinese/phy_latency/wip/gnb_loopback_n78.yml"
OUT="${OUT:-/tmp/blocking_wait_smoke}"
mkdir -p "$OUT"

if pgrep -x gnb >/dev/null; then
  echo "REFUSING: a gnb is already running (a leg may be flying)"
  exit 1
fi

echo "arm        run  banner                        handoff-samples  clean-exit  report"
printf '%.0s-' {1..86}; echo

for arm in poll block; do
  for i in $(seq 1 "$RUNS"); do
    err="$OUT/${arm}_run${i}.err"; log="$OUT/${arm}_run${i}.log"; out="$OUT/${arm}_run${i}.out"
    if [ "$arm" = block ]; then
      # The probe is ON in BOTH arms: the pair is only readable if the same ruler measured both.
      env OCUDU_PHY_BLOCKING_WAIT=1 OCUDU_UL_HANDOFF_PROBE=1 OCUDU_SCHED_VERBOSE=1 \
        "$ROOT/build/apps/gnb/gnb" -c "$ROOT/$CFG" --expert_phy.phy_pipeline gpu \
        --log.filename "$log" >"$out" 2>"$err" &
    else
      env OCUDU_UL_HANDOFF_PROBE=1 OCUDU_SCHED_VERBOSE=1 \
        "$ROOT/build/apps/gnb/gnb" -c "$ROOT/$CFG" --expert_phy.phy_pipeline gpu \
        --log.filename "$log" >"$out" 2>"$err" &
    fi
    pid=$!
    sleep "$SECS"

    # TERM, then the same five-second grace the air protocol gives the application - "clean" means the process left
    # on its own inside it, which is what makes the exit report appear at all.
    kill -TERM "$pid" 2>/dev/null
    # A child that has exited but not been waited for is a ZOMBIE, and `kill -0` succeeds on it - so the check that
    # means "the application left by itself" is the process being gone from the process table (measured 2026-10-05:
    # the loopback application leaves 5.14 s after TERM or INT, which is the shutdown's own five-second grace and is
    # the same for both arms - hence the 8 s window, wide enough to tell "slow by design" from "parked forever").
    clean="no"
    for t in $(seq 1 32); do
      st=$(ps -o state= -p "$pid" 2>/dev/null | tr -d ' ')
      if [ -z "$st" ] || [ "$st" = "Z" ]; then clean="yes"; exit_s=$((t * 25 / 100)); break; fi
      sleep 0.25
    done
    kill -9 "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null

    banner=$(grep -h 'Lower PHY worker wait:' "$out" "$err" 2>/dev/null | tail -1 | sed 's/^Lower PHY worker wait: //')
    [ -n "$banner" ] || banner="(MISSING)"
    samples=$(grep -h '\[ul_handoff\] rx_to_ul:' "$err" | tail -1 | sed -n 's/.*rx_to_ul: n=\([0-9]*\).*/\1/p')
    [ -n "$samples" ] || samples=0
    if grep -q 'contract MET' "$err"; then report="MET"; elif grep -q 'contract' "$err"; then report="see-log"; else report="none"; fi

    printf '%-8s  %-4s %-30s %-16s %-11s %s\n' "$arm" "$i" "${banner:0:30}" "$samples" "$clean" "$report"
  done
done

echo
echo "readings in $OUT (*.err, *.out). PASS requires: the block arm names the condition variable, both arms report"
echo "handoff samples, and every run exits clean on TERM (within 8 s - the application's own grace is 5.14 s)."
