#!/usr/bin/env bash
# ============================================================================
# run_training.sh - one-shot training: capture directory -> pair/rebuild/split/fine-tune/convert -> ai_assets
#
# Flow (entirely offline, no sudo):
#   pair_capture -> build_labels(candidate content check + quality gate) -> split_labels(-13 gate)
#   -> train_pad(--norm 0.15 mirrors the runtime scale, initialised from the synthetic baseline in init_models)
#   -> convert_coreml + coremlcompiler -> installed into ai_assets/
#
# usage:
#   tools/run_training.sh <capture dir> [--name NAME] [--gate DB] [--epochs N] [--lr R] [--prb 52|106]
#     --name   model name suffix (default real_<dir name>; installed as helena_pusch<prb>_<name>_ml.mlmodelc)
#     --gate   post-hoc quality gate (default -13; tighten to -15 when the labels are noisy)
#     --prb    bucket width (52 or 106)
#
# Requires: $VENV_PY (default ~/ai_ce_work/venv/bin/python, with numpy/tf/tf-keras/coremltools)
#           and the macOS xcrun coremlcompiler. The training environment is known to work (ai_train is self-contained).
#
# Output: ai_assets/helena_pusch<prb>_<name>_ml.mlmodelc + how to use it (the A/B commands).
# ============================================================================
set -u

AI_TRAIN="$(cd "$(dirname "$0")/.." && pwd)"
REPO="$(git -C "$AI_TRAIN" rev-parse --show-toplevel 2>/dev/null)"
REPO="${REPO:-$AI_TRAIN/../../../..}"
VENV_PY="${VENV_PY:-$HOME/ai_ce_work/venv/bin/python}"
ASSETS="$REPO/lib/phy/upper/signal_processors/channel_estimator/metal/ai_assets"
CAP=${1:?usage: run_training.sh <capture dir> [--name NAME] [--gate DB] [--epochs N] [--lr R] [--prb 52|106]}
shift
NAME=""; GATE=-13; EPOCHS=6; LR=1e-5; PRB=52
while [[ $# -gt 0 ]]; do
  case "$1" in
    --name) NAME=$2; shift 2 ;;
    --gate) GATE=$2; shift 2 ;;
    --epochs) EPOCHS=$2; shift 2 ;;
    --lr) LR=$2; shift 2 ;;
    --prb) PRB=$2; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 1 ;;
  esac
done
[[ -d "$CAP" ]] || { echo "capture directory does not exist: $CAP" >&2; exit 1; }
[[ -z "$NAME" ]] && NAME="real_$(basename "$CAP")"
INIT="$AI_TRAIN/init_models/helena_pusch${PRB}_sm_hi"
BUCKET=(); [[ "$PRB" == "106" ]] && BUCKET=(--bucket 106)
NSC=$((PRB * 12))

echo "== [1/5] pairing (slot exact + candidates) =="
"$VENV_PY" "$AI_TRAIN/pair_capture.py" "$CAP" --out "$CAP/pairs_v2.npz"

echo "== [2/5] label rebuild (candidate content check, gate -10) =="
"$VENV_PY" "$AI_TRAIN/build_labels.py" "$CAP" --pairs "$CAP/pairs_v2.npz" \
  --out "$CAP/labels_all.npz" "${BUCKET[@]}"

echo "== [3/5] split (post-hoc gate $GATE dB) =="
"$VENV_PY" "$AI_TRAIN/split_labels.py" "$CAP/labels_all.npz" "$CAP/realtrain.npz" \
  --gate "$GATE"

echo "== [4/5] fine-tune (--norm 0.15 mirrors the runtime scale, init=$INIT, lr=$LR, epochs=$EPOCHS) =="
"$VENV_PY" "$AI_TRAIN/train_pad.py" "$CAP/realtrain.npz" \
  "$CAP/helena_pusch${PRB}_sm_${NAME}" \
  --prb "$PRB" --init "$INIT" --epochs "$EPOCHS" --batch 32 --lr "$LR" --norm 0.15

echo "== [5/5] conversion + install =="
"$VENV_PY" "$AI_TRAIN/convert_coreml.py" "$CAP/helena_pusch${PRB}_sm_${NAME}" \
  "$CAP/helena_pusch${PRB}_${NAME}.mlpackage" --shape "$NSC"
rm -rf "$CAP/mlc_out"
xcrun coremlcompiler compile "$CAP/helena_pusch${PRB}_${NAME}.mlpackage" "$CAP/mlc_out"
DEST="$ASSETS/helena_pusch${PRB}_${NAME}_ml.mlmodelc"
rm -rf "$DEST"
cp -R "$CAP/mlc_out/helena_pusch${PRB}_${NAME}.mlmodelc" "$DEST"
echo
echo "model installed: $DEST"
echo
echo "== how to use it (A/B) =="
echo "gnb default CE = cpu (classical); run a helena leg with this model:"
echo "  sudo ./build/apps/gnb/gnb -c configs/gnb_uhd_oaiue.yaml \\"
echo "    expert_phy --pusch_ldpc_decoder_type auto --pusch_channel_estimator_algo helena \\"
echo "    --pusch_channel_estimator_helena_model_path_$PRB $DEST"
echo "comparison and verdict: tools/run_compare.sh <this leg's gnb log> (SINR comparability check + per-rnti split)"
