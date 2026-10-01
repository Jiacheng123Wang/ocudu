#!/usr/bin/env bash
# Observe the THREADS of a running leg: the macOS scheduler's own view of the process.
#
# WHY THIS EXISTS (dev doc macos_thread_priority/thread_priority_optimization_design_and_implementation.md, P0).
# The PHY probes only ever see the SYMPTOM (a max in some series). The platform exposes the layer underneath:
# QoS billing and the effective ceiling, scheduler wait time, P/E placement, context-switch and wakeup rates, and
# the per-thread stacks. Until `taskinfo` was run by hand (2026-10-01) nobody had looked, and it immediately showed
# something the code assumed otherwise: UI/IN QoS billing 0 s with `eff qos ceiling = THREAD_QOS_LEGACY`.
# This script makes those readings part of every leg instead of an emergency reflex.
#
# ⚠⚠ THE OBSERVER PERTURBS THE OBSERVED - measured, 2026-10-01. Leg `p182-n78-stress_1001_1726` (23 minutes,
# stamped c1b59d0aa5) was observed with taskinfo + powermetrics + `sample` six minutes in. It is the ONLY leg in
# the whole session that lost samples (`radio sample continuity: 1 gaps`, `1 radio receive overflow`) and its tails
# are two orders of magnitude worse than any unobserved leg: `[ul_rx_wait]` max 153 ms (unobserved: 2.2 ms),
# `[ul_channel_estimation]` max 13.3 ms (unobserved: 1.2 ms), `[dl_tx_slack] AT/BELOW 0 = 709` (unobserved: 1),
# `min = -167.7 ms`. `sample` SUSPENDS the target process to walk its stacks; powermetrics reads the scheduler.
# So: never run the heavy set on an acceptance/delivery leg - it invalidates it.
#
# usage:  bash wip/observe_threads.sh <leg-label> <phase>            # LIGHT: ps -M only (safe on any leg)
#         bash wip/observe_threads.sh <leg-label> <phase> --heavy    # + taskinfo/powermetrics/sample: DIAGNOSTIC LEGS ONLY
# output: work_tmp/obs_<label>_<phase>.{psM,taskinfo,powermetrics,sample}.txt
#
# \note It needs root for the heavy set. Without root, or without --heavy, it prints what it skipped and writes a
#       SKIPPED marker - a silent skip would make "not measured" look like "nothing to report" (the same rule the
#       probes follow for an empty worst-K list).
set -u

LABEL="${1:-}"
PHASE="${2:-}"
HEAVY=0
for a in "$@"; do [[ $a == --heavy ]] && HEAVY=1; done
if [[ -z $LABEL || -z $PHASE ]]; then
  echo "usage: bash wip/observe_threads.sh <leg-label> <phase:start|mid|end> [--heavy]" >&2
  exit 2
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$(cd "$HERE/.." && pwd)/work_tmp"
mkdir -p "$OUT"
BASE="$OUT/obs_${LABEL}_${PHASE}"

PID="$(pgrep -f 'build/apps/gnb/gnb' | head -1)"
if [[ -z $PID ]]; then
  echo "[observe] no gnb process found (pgrep -f 'build/apps/gnb/gnb')" >&2
  exit 3
fi

echo "[observe] label=$LABEL phase=$PHASE pid=$PID -> $BASE.*"

# ps -M needs no root and gives the per-thread CPU totals (the cheapest "who is burning CPU").
ps -M "$PID" >"$BASE.psM.txt" 2>&1 || echo "[observe] ps -M failed" >&2

if [[ $HEAVY -eq 0 ]]; then
  echo "SKIPPED (light mode): taskinfo, powermetrics, sample - pass --heavy on a DIAGNOSTIC leg if you need them" \
    >"$BASE.SKIPPED.txt"
  echo "[observe] light mode: ps -M only. The heavy set perturbs a leg (see the header) - use --heavy deliberately." >&2
  exit 0
fi

if [[ $EUID -ne 0 ]]; then
  echo "SKIPPED (needs root): taskinfo, powermetrics, sample" >"$BASE.SKIPPED.txt"
  echo "[observe] SKIPPED taskinfo/powermetrics/sample: not root (see $BASE.SKIPPED.txt)" >&2
  exit 0
fi

echo "[observe] ⚠ HEAVY set: this leg is a DIAGNOSTIC leg, not evidence for acceptance (it will show stalls and" >&2
echo "[observe]   possibly gaps/rx_overflows that the observation produced - dev doc §10.3)." >&2

# The scheduler's own view: QoS billing, the effective ceiling, P/E time, csw/wakeups/mach messages.
taskinfo "$PID" >"$BASE.taskinfo.txt" 2>&1 || echo "[observe] taskinfo failed" >&2

# Per-process QoS tiers, scheduler wait time and P/E placement. -n 3 gives a short trend, not one snapshot.
powermetrics --samplers tasks --show-process-qos-tiers --show-process-wait-times --show-process-amp -n 3 \
  >"$BASE.powermetrics.txt" 2>&1 || echo "[observe] powermetrics failed" >&2

# Per-thread stacks: which thread is where, and in which frame (UHD recv/send, Metal commit, the host transforms).
sample "$PID" 3 -file "$BASE.sample.txt" >/dev/null 2>&1 || echo "[observe] sample failed" >&2

echo "[observe] wrote $BASE.{taskinfo,powermetrics,sample,psM}.txt"
