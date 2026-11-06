#!/bin/bash
# Fly the gNB with one depth3 bundle and keep BOTH streams in one file, so the run can be read afterwards.
#
# WHY A WRAPPER. The probe summaries -- the phase latencies, the block table, the depth3 timing line -- go to stdout and
# stderr, not to the logger's file, so a plain redirection of the logger keeps them out of reach and the only record of a
# flight is what the person at the keyboard pastes. This wrapper tees both streams to a named log, prints the path at the
# end, and names the run after the model so the files stay distinguishable.
#
# stdout+stderr are merged in order THROUGH THE SAME PIPE, so the interleaving is what the terminal showed. `2>&1 | tee`
# would be correct only because tee is on the right of the merge; writing it as `> file 2>&1` first is what scrambles it.
set -u
NAME=${1:?usage: fly.sh <bundle-name> [config] [seconds]}
CFG=${2:-$HOME/dev/ocudu/configs/pinned_mcs04.yml}
SECS=${3:-0}
BUNDLE=$HOME/ai_depth3/gate_ai_models/bundles/$NAME/$NAME.mlmodelc
[ -d "$BUNDLE" ] || { echo "no such bundle: $BUNDLE" >&2; exit 1; }
OUTDIR=$HOME/ai_depth3/flight_logs
mkdir -p "$OUTDIR"
STAMP=$(date +%Y%m%d_%H%M%S)
LOG=$OUTDIR/${NAME}_${STAMP}.log
echo "log: $LOG"
echo "model: $NAME"
echo "config: $CFG"
# macOS ships no `timeout`; gtimeout from coreutils is used when present, and otherwise the run is untimed and says so
# rather than failing with "command not found" halfway through a flight.
TIMER=""
if [ "$SECS" -gt 0 ]; then
  if command -v gtimeout >/dev/null 2>&1; then TIMER="gtimeout $SECS"
  elif command -v timeout >/dev/null 2>&1; then TIMER="timeout $SECS"
  else echo "note: no timeout/gtimeout here, running untimed (Ctrl-C to stop)" >&2; fi
fi
# shellcheck disable=SC2086
$TIMER sudo "$HOME/dev/ocudu/build/apps/gnb/gnb" -c "$CFG" \
  --expert_phy.phy_pipeline npu \
  --expert_phy.pusch_receiver_model "$BUNDLE" 2>&1 | tee "$LOG"
echo "=== saved: $LOG"
