#!/usr/bin/env bash
# ONE protocol for BOTH legs of a pair: the traffic cue and the disturbance window are anchored to the LEG's own
# start, and every parameter is a constant in this file - the command line carries nothing but the label.
#
# WHY (dev doc 11.55). p271-dual was flown against p270 with "the same short protocol", and the pair turned out
# not to be a pair at all: hops -31.9%, payload per hop +28.3%, and the disturbance proxies 3.4x apart (the
# disturbance had landed at the START of one leg and at the END of the other). None of that was visible until
# pair_check ran, after two OTA legs had been paid for. The lesson is not "be careful": it is that a protocol
# kept in an operator's memory cannot be reproduced, so it has to be an artifact that RUNS the schedule and
# WRITES DOWN what it did.
#
# WHAT THIS DOES
#   1. waits for the leg to exist (the runner writes its provenance into <leg>.log.stderr as its first output);
#   2. waits PRE seconds and then prints a single, loud TRAFFIC NOW line - the cue - and records its instant;
#   3. DIST_DELAY seconds after the cue, starts disturbance2.sh with a FIXED burner count and duration.
#      NO --watch: the adaptive form backs burners off when the radio complains, which is exactly the two legs
#      ending up with different disturbance levels;
#   4. takes a mid-arm CPU reading while the disturbance runs;
#   5. writes <leg>.protocol.txt next to the leg's logs: the cue instant, the exact commands, the window, the
#      CPU reading - so the pair's identity can be CHECKED afterwards instead of assumed.
#
# The traffic itself is started by the operator, behind the UE (the uplink is what these legs study), because
# the sender has to be on the far side of the radio link. That is the one step this script cannot do for you -
# hence the cue. Run the IDENTICAL iperf3 command in both legs:
#
#     iperf3 -u -b 30M -l 1400 -R -P 4 -t 180 -c 10.45.0.21
#
# WHY UDP SATURATED (plan doc 11.58): `-b` is IGNORED for TCP, so the offered rate was never controlled and the
# payload per hop floated +-26% between legs - the confound that voided three rounds. 30 Mbit/s is well above what
# this link delivers (~10-13 Mbit/s), so the UE's buffer stays full and each slot's transport block is decided by
# the available PRBs and the pinned MCS instead of by TCP's bursts.
#
# USAGE
#   terminal 1:  sudo -E LEG_CONFIG=... bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu <label> ...
#   terminal 2:  bash doc_chinese/macos_thread_priority/wip/leg_protocol_driver.sh <label>
#   then start iperf3 when the cue appears. Stop the leg with Ctrl-C (SIGINT), NOT kill: run_leg.sh documents
#   that SIGTERM skips the shutdown report block.
#
# The env overrides exist ONLY to smoke-test this script offline (a loopback leg with PRE=2 and a 3 s
# disturbance); a flight must use the defaults, which are the pair's protocol.
set -u

LABEL="${1:?leg label, exactly as passed to run_leg.sh (e.g. p272-triple)}"
PRE="${PRE:-30}"                 # leg start -> traffic cue
DIST_DELAY="${DIST_DELAY:-60}"   # traffic cue -> disturbance start
DIST_N="${DIST_N:-16}"           # burners (the workflow's usual 16 on 14 cores)
DIST_SECS="${DIST_SECS:-60}"
TRAFFIC_SECS="${TRAFFIC_SECS:-180}"
# THE TRAFFIC CUE. Small packets on purpose (user, 2026-10-05): the question this workflow is on now is the TAIL,
# and a saturated iperf3 upload answers a different one - it measures throughput and is tolerant of a late packet.
# `ping` puts one small uplink packet (the echo reply) on the wire and times it, so the pipeline is nearly idle and
# a late one shows up as the RTT max.
#
# TWO CORRECTIONS FROM THE OPERATOR (2026-10-05), both kept because they change what the reading means:
#   * the ping runs from the CORE side, not from this Mac: the Mac is only the gNB/RAN in the middle of the path, so
#     the command below is to be run on the CN host (the same side the iperf3 client ran on). The macOS `ping`'s
#     refusal of -i below 0.2 s therefore has nothing to do with this cue - it was my mistake to bring it up;
#   * the interval is 0.1 s (10 Hz), not 0.02: 20 ms between packets is dense enough to block, and a rate that
#     changes the load defeats the purpose of a probe that is supposed to leave the pipeline nearly idle.
# Consequence to keep in mind when reading: at 10 Hz a 180 s window is ~1800 packets, so the PHY side sees ~1800
# [ul_pipeline] samples instead of 108k - the MAX is still the reading (it is what a late packet shows up in), but
# the fine percentiles are thin, and a single late packet is a single sample.
TRAFFIC_CUE="${TRAFFIC_CUE:-ping -i 0.1 -c 1800 <UE-IP, e.g. 10.45.0.21>   (run it on the CORE side, not on this Mac)}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"

# The leg's directory is whichever of the two the operator's LEG_LOGDIR pointed at; the runner's own default is
# the phy_pipeline_gpu one, the macos workstream keeps its own. Both are searched, and the newest match wins.
# The search path is overridable so this script can be smoke-tested WITHOUT writing a fake leg into an evidence
# directory (LEG_LOG_DIRS=/tmp/...); a flight never sets it, so a flight always searches the two real ones.
LEG_LOG_DIRS="${LEG_LOG_DIRS:-$ROOT/doc_chinese/phy_pipeline_gpu/wip/logs:$ROOT/doc_chinese/macos_thread_priority/wip/logs}"

find_leg() {
  local dir f
  for dir in ${LEG_LOG_DIRS//:/ }; do
    f=$(ls -t "$dir"/gnb_*_${LABEL}_*.log.stderr 2>/dev/null | head -1)
    [ -n "$f" ] && { printf '%s\n' "$f"; return 0; }
  done
  return 1
}

echo "leg_protocol_driver: waiting for leg '$LABEL' to appear..."
# The runner names the log by PIPELINE MODE (gnb_gpu_/gnb_cpu_/gnb_cpu_gpu_): the first version of this script
# hard-coded gnb_gpu_ and therefore never found a cpu-mode leg (p284, 2026-10-05) - the cue and the audit were
# reported missing for a leg that had run perfectly well.
LEG=""
for _ in $(seq 1 "${LEG_WAIT:-120}"); do
  if LEG=$(find_leg); then
    # The provenance block is the runner's FIRST write, so seeing it means the leg has started - and seeing
    # `[leg] regime=` means the runner got as far as printing it, not merely creating the file.
    grep -aq '^\[leg\] regime=' "$LEG" 2>/dev/null && break
  fi
  LEG=""
  sleep 1
done
if [ -z "$LEG" ]; then
  echo "REFUSING: no leg '$LABEL' started within ${LEG_WAIT:-120} s (looked for gnb_*_${LABEL}_*.log.stderr)." >&2
  exit 2
fi
LOGDIR="$(dirname "$LEG")"
AUDIT="$LOGDIR/$(basename "$LEG" .log.stderr).protocol.txt"

if ! pgrep -x gnb >/dev/null; then
  echo "WARNING: no gnb process is running - did the leg already stop?" >&2
fi

echo "leg  : $LEG"
echo "plan : leg start -> ${PRE}s cue -> iperf3 -t ${TRAFFIC_SECS} -P 4 (you start it) -> +${DIST_DELAY}s disturbance ${DIST_N} burners ${DIST_SECS}s"

sleep "$PRE"
CUE=$(date +%s)
STAMP=$(date '+%Y-%m-%d %H:%M:%S')
echo
echo "  ============================================================"
echo "   TRAFFIC NOW  ($STAMP)   ->   $TRAFFIC_CUE"
echo "  ============================================================"
echo

sleep "$DIST_DELAY"
# DIST_N=0 is the QUIET arm of a pair: no disturbance, but the cue and the audit file still happen, so a leg
# flown without load is still a leg whose protocol is on disk (the p272/p273 pair had no audit at all, and the
# missing disturbance had to be inferred from the readings afterwards - which worked, but only in hindsight).
if [ "$DIST_N" -eq 0 ]; then
  D0=$(date +%s); D1=$D0
  MID=$(top -l 1 -n 0 2>/dev/null | grep '^CPU usage' | tail -1)
  echo "[$(date '+%H:%M:%S')] quiet arm: no disturbance (DIST_N=0); mid-arm CPU: ${MID:-<none>}"
  {
    echo "leg            : $LABEL"
    echo "leg log        : $LEG"
    echo "traffic cue    : $STAMP (epoch $CUE), i.e. leg start + ${PRE}s"
    echo "traffic        : $TRAFFIC_CUE   (started by the operator at the cue)"
    echo "disturbance    : NONE (quiet arm, DIST_N=0)"
    echo "mid-arm CPU    : ${MID:-<none>}"
    echo "leg stop       : Ctrl-C (SIGINT) - SIGTERM skips the shutdown report block (see run_leg.sh)"
  } >"$AUDIT"
  echo
  echo "[$(date '+%H:%M:%S')] protocol written to: $AUDIT"
  echo "Let iperf3 run out (${TRAFFIC_SECS}s from the cue), then stop the leg with Ctrl-C."
  exit 0
fi
D0=$(date +%s)
echo "[$(date '+%H:%M:%S')] starting disturbance: $DIST_N burners for ${DIST_SECS}s (no --watch: fixed N by construction)"
bash "$HERE/disturbance2.sh" start "$DIST_N" "$DIST_SECS" >"$LOGDIR/$(basename "$LEG" .log.stderr).disturbance.log" 2>&1 &
DIST_PID=$!

# One mid-arm reading of the whole machine, while the disturbance is up (the workflow's own check that the arm
# ran under the load it claims). Taken in the BACKGROUND on purpose: `top -l 3` takes ~2-3 s to answer, and
# waiting for it in the foreground made the recorded window longer than the disturbance itself (measured on the
# offline self-test: a 3 s disturbance was written down as 5 s).
MIDF="$LOGDIR/$(basename "$LEG" .log.stderr).midarm.txt"
( sleep $((DIST_SECS / 3 + 1)); top -l 3 -n 0 2>/dev/null | grep '^CPU usage' | tail -1 >"$MIDF" ) &
MID_PID=$!

wait "$DIST_PID" 2>/dev/null
D1=$(date +%s)
wait "$MID_PID" 2>/dev/null
MID=$(cat "$MIDF" 2>/dev/null)
echo "[$(date '+%H:%M:%S')] mid-arm: ${MID:-<no top reading>}"

{
  echo "leg            : $LABEL"
  echo "leg log        : $LEG"
  echo "traffic cue    : $STAMP (epoch $CUE), i.e. leg start + ${PRE}s"
  echo "traffic        : iperf3 -u -b 30M -l 1400 -R -P 4 -t ${TRAFFIC_SECS} -c <server>   (started by the operator at the cue)"
  echo "disturbance    : disturbance2.sh start $DIST_N $DIST_SECS   (no --watch)"
  echo "disturbance at : traffic cue + ${DIST_DELAY}s, ran $((D1 - D0))s (epoch $D0..$D1)"
  echo "mid-arm CPU    : ${MID:-<none>}"
  echo "leg stop       : Ctrl-C (SIGINT) - SIGTERM skips the shutdown report block (see run_leg.sh)"
} >"$AUDIT"

echo
echo "[$(date '+%H:%M:%S')] disturbance finished. Protocol written to:"
echo "  $AUDIT"
echo "Let iperf3 run out (${TRAFFIC_SECS}s from the cue), then stop the leg with Ctrl-C."
