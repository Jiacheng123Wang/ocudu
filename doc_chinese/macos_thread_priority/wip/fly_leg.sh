#!/usr/bin/env bash
# ONE COMMAND = ONE LEG *WITH ITS PROTOCOL*.  usage:  sudo -E bash fly_leg.sh <label> <triple|dual> <quiet|stress> [cpu|cpu_gpu|gpu]
#
# WHY THIS EXISTS (dev doc 11.61). Two sessions in a row were flown with the protocol driver sitting on a separate
# line of the instructions, and two sessions in a row it did not run: no traffic cue at a recorded instant, no
# disturbance (`late max` 0.95-2.7 ms against the 11 ms a real 16-burner window produces), no .protocol.txt. The
# legs were still readable - hops and defer99 matched - but the question they were flown for (does the merge
# survive CPU competition?) went unanswered twice, because the artifact that APPLIES the load was optional in
# practice. A protocol that can be skipped will be skipped, so here it is not optional: this script starts the
# driver, runs the leg, and REFUSES TO CALL THE LEG GOOD if the protocol artifact is missing.
#
# It also pins everything else the pair needs, so the only thing that changes between two legs is the profile:
#   config   gnb_pinned_mcs13.yml   (MCS 13 = 16QAM: the 64QAM comparison legs kept coming out marginal)
#   knobs    the standard instrument set, hand-off probe included
#   traffic  cued at leg start + 30 s, one identical iperf3 command per leg (printed at the cue)
#   load     quiet: none.  stress: 16 user-space burners for 60 s, starting 60 s after the cue, same for both legs.
#
# The ONE step a script cannot do is start iperf3: the sender has to be behind the UE, on the far side of the
# radio link. That is what the cue is for.
set -u

LABEL="${1:?usage: fly_leg.sh <label> <triple|dual> <quiet|stress> [cpu|cpu_gpu|gpu]}"
PROFILE="${2:?profile: triple or dual}"
LOAD="${3:-quiet}"
MODE="${4:-gpu}"
case "$PROFILE" in triple|dual) ;; *) echo "refusing profile '$PROFILE' (triple|dual)" >&2; exit 2 ;; esac
case "$LOAD" in quiet|stress) ;; *) echo "refusing load '$LOAD' (quiet|stress)" >&2; exit 2 ;; esac
case "$MODE" in cpu|cpu_gpu|gpu) ;; *) echo "refusing mode '$MODE' (cpu|cpu_gpu|gpu)" >&2; exit 2 ;; esac

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
CFG="${LEG_CFG:-$ROOT/doc_chinese/macos_thread_priority/wip/gnb_pinned_mcs13.yml}"
DRV="$HERE/leg_protocol_driver.sh"
RUN="${RUN_LEG:-$ROOT/doc_chinese/phy_pipeline_gpu/wip/run_leg.sh}"
# WHERE THE LEGS LAND. The workstream moved to doc_chinese/macos_thread_priority/, and until 2026-10-05 this
# script kept the runner's own default (doc_chinese/phy_pipeline_gpu/wip/logs) - because it never PASSED a log
# directory to the runner at all, only used one for its own audit check. New legs therefore went to the old
# workstream's directory while every reader here (ul_health.sh, leg_protocol_driver.sh, pair_check.sh) had
# already been pointed at both. The default is now this workstream's, and LEG_LOGDIR is handed to the runner
# explicitly, so where a leg is written is decided HERE and not by a default two workstreams away.
# The legs already flown stay where they are: they are evidence, the documents name their paths, and every reader
# searches both directories (LEG_LOG_DIRS).
LOGDIR="${LEG_LOGDIR:-$ROOT/doc_chinese/macos_thread_priority/wip/logs}"

[ -f "$CFG" ] || { echo "refusing: missing $CFG" >&2; exit 2; }
[ -f "$DRV" ] || { echo "refusing: missing $DRV" >&2; exit 2; }
[ -f "$RUN" ] || { echo "refusing: missing $RUN" >&2; exit 2; }

if pgrep -x gnb >/dev/null; then
  echo "REFUSING: a gnb is already running (kill it with Ctrl-C / kill -INT first)." >&2
  exit 2
fi

if [ "$LOAD" = stress ]; then
  DIST_N=${DIST_N_BURNERS:-16}; REGIME=stress
else
  DIST_N=0;  REGIME=default
fi

# OCUDU_UL_LANE_GRID=1 IS PART OF THE SET (2026-10-05): until today it was not, and the [lane_grid] report
# therefore never appeared in an air leg at all - checked on p285, zero lines - which also meant that the ABSOLUTE
# lag and its per-second window minima (the permanent-backlog detector, dev doc 11.70) would have been silent.
# With the paced-lane/paced-commit knobs off the grid is an OBSERVER: it notes the frontier, fits the rate and
# reports; the only thing it can move is the paced gate, which is not in this set.
KNOBS=(OCUDU_SCHED_VERBOSE=1 OCUDU_UL_PHASE_SEGMENTS=1 OCUDU_UL_SLOT_GRID=1 OCUDU_UL_STABILITY_WINDOWS=8
       OCUDU_UL_THREAD_CPU=1 OCUDU_UL_TIMING_EVENTS=16 OCUDU_UL_WATCHDOG=1 OCUDU_UL_HANDOFF_PROBE=1
       OCUDU_UL_LANE_GRID=1)
# EXTRA_KNOBS is how an A/B passes the ONE knob under test without editing this file:
#   EXTRA_KNOBS="OCUDU_UL_INLINE_PUSCH=1" bash fly_leg.sh p288-dual dual quiet cpu
# A flight's other leg must be flown with the SAME standard set and nothing extra, which is what makes the pair a
# pair - the wrapper prints what it will pass, so the log says which arm it was.
if [ -n "${EXTRA_KNOBS:-}" ]; then
  # shellcheck disable=SC2206
  KNOBS+=(${EXTRA_KNOBS})
fi

echo "==================================================================================="
echo " leg      : $LABEL   (profile=$PROFILE, load=$LOAD, regime=$REGIME, pipeline=$MODE)"
echo " logs     : $LOGDIR"
echo " traffic  : at the cue -> ${TRAFFIC_CUE:-sudo ping -i 0.02 -c 9000 10.45.0.21}"
echo " load     : $( [ "$DIST_N" -gt 0 ] && echo "$DIST_N user-space burners for ${DIST_SECS:-60} s, ${DIST_DELAY:-60} s after the cue" || echo "none (quiet arm)" )"
echo "==================================================================================="

mkdir -p "$LOGDIR"
( DIST_N="$DIST_N" DIST_SECS="${DIST_SECS:-60}" PRE="${PRE:-30}" DIST_DELAY="${DIST_DELAY:-60}" \
    bash "$DRV" "$LABEL" >"$LOGDIR/${LABEL}.driver.log" 2>&1 ) &
DRV_PID=$!

# SUDO is overridable ONLY so this script can be exercised offline with a stub runner (SUDO= RUN_LEG=...);
# a flight leaves it as `sudo`, which is what the operator's non-root shell needs. The array is expanded with the
# ${arr[@]+...} idiom because /bin/bash on macOS is 3.2, where an EMPTY array under `set -u` is an error - the
# first version of this file died with "-E: command not found" for exactly that reason (measured, offline test).
SUDO_BIN="${SUDO-sudo}"
LEG_ARGS=("$MODE" "$LABEL" --regime="$REGIME" --smoke=40 "${KNOBS[@]}"
          --expert_execution.threads.lower_phy.execution_profile="$PROFILE")
if [ -n "$SUDO_BIN" ]; then
  "$SUDO_BIN" -E LEG_CONFIG="$CFG" LEG_LOGDIR="$LOGDIR" bash "$RUN" "${LEG_ARGS[@]}"
else
  LEG_CONFIG="$CFG" LEG_LOGDIR="$LOGDIR" bash "$RUN" "${LEG_ARGS[@]}"
fi
rc=$?

wait "$DRV_PID" 2>/dev/null
AUDIT=$(ls -t "$LOGDIR"/gnb_*_${LABEL}_*.protocol.txt 2>/dev/null | head -1)
echo
if [ -n "$AUDIT" ]; then
  echo "PROTOCOL OK - the cue and the load window are on disk:"
  sed 's/^/  /' "$AUDIT"
  if [ "$DIST_N" -gt 0 ]; then
    DLOG=$(ls -t "$LOGDIR"/gnb_*_${LABEL}_*.disturbance.log 2>/dev/null | head -1)
    [ -n "$DLOG" ] && { echo "  (disturbance log: $DLOG)"; tail -2 "$DLOG" | sed 's/^/    /'; }
  fi
else
  echo "!! PROTOCOL MISSING for $LABEL: the driver never recorded the cue (see $LOGDIR/${LABEL}.driver.log)."
  echo "!! The leg may still be readable, but DO NOT quote a load-reading from it."
fi
exit "$rc"
