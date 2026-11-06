#!/bin/bash
# Fly the gNB with one depth3 bundle, showing the run on the console AND keeping it in a per-leg log file.
#
# ARGUMENT ORDER: leg first, because every flight has one and it decides where the log goes; everything after it has a
# sensible default, so `fly.sh <leg>` is a complete command.
#
#   fly.sh <leg> [config] [model] [seconds]
#     leg      required; logs go to ~/ai_depth3/flight_logs/<leg>/
#     config   gnb yaml                (default: configs/pinned_mcs04.yml)
#     model    bundle name             (default: none = the build's default bundle)
#     seconds  stop after N seconds    (default: 0 = unlimited)
#
# THE CONSOLE MATTER OF BUFFERING. When stdout is a pipe the app's printf is block-buffered, so `gnb ... | tee log` shows
# nothing until the buffer fills or the process exits -- which is what made the first version of this wrapper look silent
# during a flight. `script -q /dev/null` gives the app a pty, so it line-buffers again and the console shows the run as it
# happens, while tee still records every byte.
#
# THE SUMMARY IS THE POINT OF THE FLIGHT. The phase latencies ([ul_pipeline], [ul_equalization_demod], ...) and the block
# table are printed at SHUTDOWN. Stop the run with ONE Ctrl-C and wait for "Stopping..." and the summary; a second Ctrl-C
# kills the process before it reports, and the log then contains a flight with no measurements in it.
set -u
LEG=${1:-}
if [ -z "$LEG" ]; then
  sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//' >&2
  exit 2
fi
CFG=${2:-$HOME/dev/ocudu/configs/pinned_mcs04.yml}
MODEL=${3:-}
SECS=${4:-0}

GNB=$HOME/dev/ocudu/build/apps/gnb/gnb
[ -x "$GNB" ] || { echo "no gnb at $GNB" >&2; exit 1; }
[ -f "$CFG" ] || { echo "no config at $CFG" >&2; exit 1; }

ARGS=(-c "$CFG" --expert_phy.phy_pipeline npu)
if [ -n "$MODEL" ]; then
  BUNDLE=$HOME/ai_depth3/gate_ai_models/bundles/$MODEL/$MODEL.mlmodelc
  [ -d "$BUNDLE" ] || { echo "no such bundle: $BUNDLE" >&2; exit 1; }
  ARGS+=(--expert_phy.pusch_receiver_model "$BUNDLE")
fi

OUTDIR=$HOME/ai_depth3/flight_logs/$LEG
mkdir -p "$OUTDIR"
STAMP=$(date +%Y%m%d_%H%M%S)
LOG=$OUTDIR/${MODEL:-build-default}_${STAMP}.log
{
  echo "# leg:    $LEG"
  echo "# model:  ${MODEL:-<build default>}"
  echo "# config: $CFG"
  echo "# gnb:    $GNB  sha256 $(shasum -a 256 "$GNB" | cut -c1-16)"
  echo "# started $(date -Is)"
} > "$LOG"
echo "log: $LOG"

# One SIGINT after SECS seconds, so an unattended run still reaches its summary. SIGINT, never SIGKILL: the summary is
# emitted by the normal shutdown path.
if [ "$SECS" -gt 0 ]; then
  ( sleep "$SECS"; pkill -INT -f "build/apps/gnb/gnb -c" 2>/dev/null; echo "[fly] sent one SIGINT after ${SECS}s" ) &
fi

# THE DIAGNOSTICS THE RECEIVER ONLY PRINTS ON REQUEST. `OCUDU_DEPTH3_TIMING` makes the arm report its allocation mix --
# which bucket served how many receptions -- and that line is the only way to tell a model's cost apart from the cost of
# switching between its buckets. It prints and changes nothing, so flights enable it by default; the other two are passed
# through when the caller set them, because sudo does not carry the environment.
PASS_ENV=(env "OCUDU_DEPTH3_TIMING=${OCUDU_DEPTH3_TIMING:-1}")
for v in OCUDU_DEPTH3_COMPUTE_UNITS OCUDU_DEPTH3_BUCKETS OCUDU_DEPTH3_FALLBACK; do
  if [ -n "${!v:-}" ]; then PASS_ENV+=("$v=${!v}"); fi
done
# `script -q /dev/null` allocates the pty (line buffering); tee writes the file and the console at the same time.
script -q /dev/null sudo "${PASS_ENV[@]}" "$GNB" "${ARGS[@]}" 2>&1 | tee -a "$LOG"
echo "=== saved: $LOG"
if ! grep -qa "ul_pipeline" "$LOG"; then
  echo "=== WARNING: no [ul_pipeline] summary in this log -- the run was stopped before shutdown, so it holds no" >&2
  echo "            latency or block measurements. Stop with a single Ctrl-C and wait for 'Stopping...'." >&2
fi
