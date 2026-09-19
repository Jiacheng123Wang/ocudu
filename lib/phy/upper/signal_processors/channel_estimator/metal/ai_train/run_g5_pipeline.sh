#!/usr/bin/env bash
# G-5 offline pipeline sidecar: capture directory -> pairing -> label rebuild -> split -> fine-tune -> convert.
# One command runs the first half of the nightly training after a day of captures; the A/B on the real
# machine is still run by hand, as the manual's section 8 describes.
#
# usage:
#   run_g5_pipeline.sh <capture dir> [--prb 52|106] [--init <SavedModel>]
#                       [--epochs N] [--lr R] [--no-convert]
#
# Requires: a python3 venv (numpy + tensorflow + tf-keras; conversion additionally needs the macOS
#           coremltools + xcrun coremlcompiler). Point VENV_PY at the venv; defaults to the system python3.
# Self-contained: the script finds the ai_train tools relative to its own directory, no ~/ai_ce_work needed.
set -euo pipefail

AI_TRAIN=$(cd "$(dirname "$0")" && pwd)
PY=${VENV_PY:-python3}
CAP=${1:?usage: run_g5_pipeline.sh <capture_dir> [--prb 52|106] [--init <sm>] [--epochs N] [--lr R] [--no-convert]}
PRB=52; EPOCHS=6; LR=1e-5; INIT=""; CONVERT=1
shift
while [[ $# -gt 0 ]]; do
  case "$1" in
    --prb) PRB=$2; shift 2 ;;
    --init) INIT=$2; shift 2 ;;
    --epochs) EPOCHS=$2; shift 2 ;;
    --lr) LR=$2; shift 2 ;;
    --no-convert) CONVERT=0; shift ;;
    *) echo "unknown arg $1" >&2; exit 1 ;;
  esac
done
[[ -d "$CAP" ]] || { echo "capture dir not found: $CAP" >&2; exit 1; }
[[ -z "$INIT" ]] && INIT="$AI_TRAIN/init_models/helena_pusch${PRB}_sm_hi"
BUCKET=(); [[ "$PRB" == "106" ]] && BUCKET=(--bucket 106)

echo "== [1/5] pair ($PRB PRB bucket) =="
"$PY" "$AI_TRAIN/pair_capture.py" "$CAP" --out "$CAP/pairs_v2.npz"

echo "== [2/5] DD label rebuild (content-verified candidates) =="
"$PY" "$AI_TRAIN/build_labels.py" "$CAP" --pairs "$CAP/pairs_v2.npz" \
  --out "$CAP/labels_all.npz" "${BUCKET[@]}"

echo "== [3/5] train/val split (10% holdout, seed 42; tighten with the GATE env) =="
"$PY" "$AI_TRAIN/split_labels.py" "$CAP/labels_all.npz" "$CAP/realtrain.npz" \
  --gate "${GATE:--10}"

echo "== [4/5] fine-tune (init=$INIT, lr=$LR, epochs=$EPOCHS; NORM env -> --norm) =="
NORM_ARGS=()
[[ -n "${NORM:-}" ]] && NORM_ARGS=(--norm "$NORM")
"$PY" "$AI_TRAIN/train_pad.py" "$CAP/realtrain.npz" "$CAP/helena_pusch${PRB}_sm_real" \
  --prb "$PRB" --init "$INIT" --epochs "$EPOCHS" --batch 32 --lr "$LR" "${NORM_ARGS[@]}"

echo "== [5/5] convert (macOS coremltools + coremlcompiler) =="
if [[ "$CONVERT" == "1" ]]; then
  "$PY" "$AI_TRAIN/convert_coreml.py" "$CAP/helena_pusch${PRB}_sm_real" \
    "$CAP/helena_pusch${PRB}_real.mlpackage" --shape $((PRB * 12))
  rm -rf "$CAP/mlc_out"
  xcrun coremlcompiler compile "$CAP/helena_pusch${PRB}_real.mlpackage" "$CAP/mlc_out"
  echo "modelc -> $CAP/mlc_out/helena_pusch${PRB}_real.mlmodelc"
  echo "(install/rollout per AI_CE_G5_manual.md sections 7/9)"
else
  echo "skipped (--no-convert)"
fi
echo "PIPELINE-DONE"
