#!/usr/bin/env bash
# Export a trained depth-3 run and put it through the three acceptance measurements.
#
# ONE PLACE, because the three steps have to agree: the export must carry the SAME architecture
# flags the run was trained with (a wrong --c-hid exports a bundle the engine binds anyway and
# whose numbers are silently wrong), the replay must be SERIAL and with nothing else on the GPU
# (the harness's own warning, and measured again on 2026-10-18: with a trainer at ~700% CPU the
# Core ML forward fails and the arm degrades to the classical chain, which reads as a perfect
# bit agreement), and the two criteria are the ones the retrain has to meet.
#
# What it reports:
#   1. cell_report.py over both attach captures -- decode per (modulation x width x code rate),
#      with the regressions named. Baseline for the deployed ft bundle: 64QAM 5-8 PRB at code
#      rate >=0.92 is -108 on 253 first transmissions.
#   2. bit_agreement.py on that same cell -- sign agreement with the classical chain on the
#      receptions the classical chain decoded. Target >=99.5%.
#
# Usage:
#   eval_narrow_c32.sh <run-dir> [--label NAME] [--c-hid N]
set -u -o pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
PY=~/ai_ce_work/venv/bin/python
TRAIN=$ROOT/lib/phy/upper/signal_processors/channel_estimator/metal/ai_train/train_depth3.py
PROBE=$ROOT/doc_chinese/llr_ai_detection/probe
W=$ROOT/doc_chinese/llr_ai_detection/work_tmp

RUN=${1:-}
[ -n "$RUN" ] || { echo "usage: $0 <run-dir> [--label NAME] [--c-hid N]" >&2; exit 2; }
shift
LABEL=$(basename "$RUN")
C_HID=32
while [ $# -gt 0 ]; do
  case "$1" in
    --label) LABEL="$2"; shift 2 ;;
    --c-hid) C_HID="$2"; shift 2 ;;
    *) echo "unknown argument '$1'" >&2; exit 2 ;;
  esac
done

KERAS=$RUN/depth3_best.keras
BUNDLE=$RUN/$LABEL.mlmodelc
[ -f "$KERAS" ] || { echo "missing $KERAS - the run has not finished" >&2; exit 2; }
[ -x "$PY" ] || { echo "missing the TensorFlow venv at $PY" >&2; exit 2; }

echo "== export $KERAS -> $RUN/$LABEL.mlpackage"
"$PY" "$TRAIN" --export-only "$KERAS" --export-probe "$RUN/$LABEL.mlpackage" \
  --modcond --layout spatial --in-channels 12 --blocks 18 --c-hid "$C_HID" --snr-range 12,48 || {
    echo "export failed" >&2; exit 1; }
xcrun coremlcompiler compile "$RUN/$LABEL.mlpackage" "$RUN" >/dev/null || {
  echo "coremlcompiler failed" >&2; exit 1; }
[ -d "$BUNDLE" ] || { echo "expected $BUNDLE after compilation" >&2; exit 1; }
echo "  bundle: $BUNDLE"

# The THIRD set is the part of the failing leg that was NOT used to manufacture training data
# (its first two thirds supplied the 2,7,11 synthetic samples). Without it, that leg's replay would
# be in-sample -- see design section 19.211(e), which records the deviation from the pre-registered
# sample set and why it was forced.
for pair in "attach-1019a:a1019a" "attach-ai2-c32w:ai22" "HOLDOUT:ai22h"; do
  dir=${pair%%:*}; tag=${pair##*:}
  [ "$dir" = "HOLDOUT" ] && dir=$W/data/holdout/ai22
  echo "== replay $dir through $LABEL (serial)"
  captures_dir=$W/corpus/attach/$dir
  [ -d "$captures_dir" ] || captures_dir=$dir
  "$PY" "$PROBE/attach_ab.py" --captures "$captures_dir" --model "$BUNDLE" \
    --out "$W/cmp/ab_${tag}_${LABEL}.csv" > "$W/cmp/ab_${tag}_${LABEL}.txt" 2>&1 || {
      echo "replay of $dir failed" >&2; tail -5 "$W/cmp/ab_${tag}_${LABEL}.txt" >&2; exit 1; }
  grep -E "^AI arm COMPUTED|published SINR against|verdict flips on computed" "$W/cmp/ab_${tag}_${LABEL}.txt"
done

echo
echo "== criterion 1/3: decode per cell, regressions named"
"$PY" "$PROBE/cell_report.py" \
  --ab "$W/cmp/ab_a1019a_${LABEL}.csv" --ab "$W/cmp/ab_ai22_${LABEL}.csv" \
  --ab "$W/cmp/ab_ai22h_${LABEL}.csv" --label "$LABEL"

echo
echo "== the deployed bundle on the HELD-OUT slice, for a like-for-like comparison"
"$PY" "$PROBE/attach_ab.py" --captures "$W/data/holdout/ai22" \
  --model "$W/data/models/d3_modcond_ft.mlmodelc" --out "$W/cmp/ab_ai22h_ft.csv" \
  > "$W/cmp/ab_ai22h_ft.txt" 2>&1 || echo "ft holdout replay failed" >&2
"$PY" "$PROBE/cell_report.py" --ab "$W/cmp/ab_ai22h_ft.csv" --label "ft (held-out)" \
  --min-n 1000 2>&1 | head -12

echo
echo "== criterion 2: bit agreement in the failing cell (fixed population: every reception of that cell"
echo "   that the classical chain decoded, whether or not this bundle decodes it -- a metric that"
echo "   moves with the bundle is a metric that cannot be compared across bundles)"
"$PY" "$PROBE/bit_agreement.py" --ab "$W/cmp/ab_ai22_${LABEL}.csv" --include-arm-ok \
  --captures "$W/corpus/attach/attach-ai2-c32w" --model "$BUNDLE" --cell 64QAM:5-8 --limit 12 || true
echo
echo "== the same measurement for the DEPLOYED bundle, so the number has a scale"
"$PY" "$PROBE/bit_agreement.py" --ab "$W/cmp/ab_ai22_${LABEL}.csv" --include-arm-ok \
  --captures "$W/corpus/attach/attach-ai2-c32w" \
  --model "$W/data/models/d3_modcond_ft.mlmodelc" --cell 64QAM:5-8 --limit 12 || true
