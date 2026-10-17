#!/usr/bin/env bash
# One process, many receptions: the only way to see the depth-3 arm's STEADY-STATE cost.
#
# Why this exists (design document section 19.167): the arm's `[depth3-timing]` ring prints on the FIRST
# forward and then on every 500th, and the comparison harness replays ONE capture per process, so a
# one-capture-per-process run can only ever report sample 1 -- the cold one, which includes the model load
# and the first inference. `--harq-chain` keeps a single processor (and therefore a single arm and a single
# engine) alive across the whole run, so `--repeat N` with it turns N receptions of one capture into N ring
# samples in one process. At N >= 500 the ring's p50 is warm and the "sample 500" line is what to compare.
#
# Usage: soak_timing.sh <capture-base> <repeat> <model.mlmodelc|""> <tag> [extra replay args...]
#   pass "" as the model to measure the CLASSICAL chain (no AI arm at all)
# Output: tmp/soak/<tag>/{stdout,stderr}.txt, and the timing/verdict lines on this script's stdout.
set -u
W=/Users/jiachengwang/dev/ocudu/doc_chinese/llr_ai_detection/work_tmp
TOOL=${TOOL:-/Users/jiachengwang/dev/ocudu/build/lib/phy/upper/channel_processors/metal/ul_chain_replay}
CAP=$1
N=${2:-600}
MODEL=${3:-}
TAG=$4
shift 4
OUT=$W/tmp/soak/$TAG
rm -rf "$OUT"
mkdir -p "$OUT"

ENV=(OCUDU_REPLAY_RECEIVER=ai)
if [ -n "$MODEL" ]; then
  ENV+=(OCUDU_REPLAY_MODEL="$MODEL")
else
  ENV=(OCUDU_REPLAY_RECEIVER=classic)
fi

# SOAK_QOS pins the scheduling tier. This machine is BIMODAL on the HOST-side segments: the same build and
# the same capture measured deliver p50 102-134 us on one run and 507-809 us on the next, with the device
# segment only 2.2x apart while the host loops were 8x. That is a scheduling/placement effect, not a code
# effect, and an A/B taken across the flip compares nothing -- so the tier is chosen explicitly and recorded
# in the tag line. Default is the plain (unpinned) run, which is what every earlier measurement used.
SOAK_QOS=${SOAK_QOS:-}
QOS_PREFIX=()
if [ -n "$SOAK_QOS" ]; then
  QOS_PREFIX=(taskpolicy -t "$SOAK_QOS")
fi

echo "=== $TAG: $(basename "$CAP") x$N, model=${MODEL:-<classical>} qos=${SOAK_QOS:-<default>} $*"
start=$(date +%s)
env "${ENV[@]}" "${QOS_PREFIX[@]}" nice -n 15 "$TOOL" "$CAP" --metal --out "$OUT/replay" \
  --harq-chain --repeat "$N" "$@" > "$OUT/stdout.txt" 2> "$OUT/stderr.txt"
rc=$?
stop=$(date +%s)
echo "    exit=$rc wall=$((stop - start))s"
# The arm's lines go to stderr; the chain's verdict lines go to stdout.
grep -a 'depth3-timing' "$OUT/stderr.txt" | sed 's/^/    /'
grep -aE '^crc=|degraded|depth3-measure|depth-3 receiver (arm|model)' "$OUT/stdout.txt" | head -8 | sed 's/^/    /'
echo "    (full logs in $OUT)"
