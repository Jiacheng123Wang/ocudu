#!/usr/bin/env bash
# Take a finished training run anywhere on the network and carry it through export, compile and the three gates.
#
# WHY THIS EXISTS (design section 19.439). The training machines are Linux and cannot produce a Core ML bundle: `coremltools` writes an
# `.mlpackage` anywhere, but compiling it to `.mlmodelc` needs `xcrun coremlcompiler`, which only exists on macOS, and the gates need
# `ul_chain_replay` driving the real receiver over the captured grids. So the hook cannot run where the training runs. What can be automated
# is everything after the checkpoint leaves that machine: copy it here, export, compile, gate, and print the verdict. One command per run.
#
# Usage: finish_run.sh <user@host:/path/to/run-dir> <name> [reference-keras]
#   e.g. finish_run.sh jwang@192.168.31.107:/home/jwang/work/ai_phy_train/runs/e9_1024 e9
set -euo pipefail

SRC=${1:?usage: finish_run.sh <user@host:run-dir> <name> [reference.keras]}
NAME=${2:?usage: finish_run.sh <user@host:run-dir> <name> [reference.keras]}
HERE=$(cd "$(dirname "$0")" && pwd)
# THE ROOT COMES FROM GIT, NOT FROM COUNTING `..` (design section 19.460): counting directories put the working tree under lib/, so every
# artefact was written to lib/doc_chinese/... instead of doc_chinese/..., and the interpreter resolved to whatever happened to be first on PATH.
ROOT=$(git -C "$HERE" rev-parse --show-toplevel)
WORK=$ROOT/doc_chinese/llr_ai_detection/work_tmp
PY=$HOME/ai_ce_work/venv/bin/python
REF=${3:-$WORK/data/runs/d3_e4_real_1023_best.keras}
KERAS=$WORK/data/runs/${NAME}_best.keras
OUT=$WORK/artifacts/buckets/$NAME

echo "== $NAME: copying the checkpoint from $SRC"
mkdir -p "$WORK/data/runs" "$OUT/buckets"
rsync -a "$SRC/depth3_best.keras" "$KERAS"

echo "== $NAME: exporting"
EXTRA=()
# A bundle trained with the reporting heads keeps them; one trained without must be exported without, or the arm will look for outputs
# that do not exist.
if grep -q -- "--no-reporting-heads" "$WORK/data/runs/${NAME}_cmdline.txt" 2>/dev/null; then
  EXTRA=(--no-reporting-heads)
fi
"$PY" -u "$HERE/train_depth3.py" --export-only "$KERAS" --export-probe "$OUT/$NAME.mlpackage" \
    --export-buckets 108,180,252,324,396,468,540,612 --modcond --layout spatial \
    --modulation 256QAM --snr-range 0,48 ${EXTRA[@]+"${EXTRA[@]}"}

echo "== $NAME: compiling"
xcrun coremlcompiler compile "$OUT/$NAME.mlpackage" "$OUT" >/dev/null
for width in 108 180 252 324 396 468 540 612; do
  xcrun coremlcompiler compile "$OUT/${NAME}_w${width}.mlpackage" "$OUT" >/dev/null
  mv "$OUT/${NAME}_w${width}.mlmodelc" "$OUT/buckets/base_w${width}.mlmodelc"
done

echo "== $NAME: gates"
"$PY" -u "$WORK/probe/verdict.py" --mlmodelc "$OUT/$NAME.mlmodelc" --keras "$KERAS" \
      --reference "$REF" --captures 40
