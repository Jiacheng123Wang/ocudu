#!/usr/bin/env bash
# ============================================================================
# run_training.sh — 一键训练：采集目录 → 配对/重建/划分/微调/转换 → ai_assets
#
# 流程（全部离线，无需 sudo）：
#   pair_capture → build_labels(候选内容验证+质量门) → split_labels(-13 门)
#   → train_pad(--norm 0.15 镜像运行时尺度, 从 init_models 的合成基线初始化)
#   → convert_coreml + coremlcompiler → 入库 ai_assets/
#
# 用法:
#   tools/run_training.sh <采集目录> [--name NAME] [--gate DB] [--epochs N] [--lr R] [--prb 52|106]
#     --name   模型名后缀（默认 real_<目录名>；入库名 helena_pusch<prb>_<name>_ml.mlmodelc）
#     --gate   后置质量门（默认 -13；标签噪声大时收紧到 -15）
#     --prb    桶宽（52 或 106）
#
# 依赖：$VENV_PY（默认 ~/ai_ce_work/venv/bin/python，含 numpy/tf/tf-keras/coremltools）
#       与 macOS 的 xcrun coremlcompiler。训练环境已验证可工作（ai_train 自包含）。
#
# 输出：ai_assets/helena_pusch<prb>_<name>_ml.mlmodelc + 使用说明（A/B 命令）。
# ============================================================================
set -u

AI_TRAIN="$(cd "$(dirname "$0")/.." && pwd)"
REPO="$(git -C "$AI_TRAIN" rev-parse --show-toplevel 2>/dev/null)"
REPO="${REPO:-$AI_TRAIN/../../../..}"
VENV_PY="${VENV_PY:-$HOME/ai_ce_work/venv/bin/python}"
ASSETS="$REPO/lib/phy/upper/signal_processors/channel_estimator/metal/ai_assets"
CAP=${1:?用法: run_training.sh <采集目录> [--name NAME] [--gate DB] [--epochs N] [--lr R] [--prb 52|106]}
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
[[ -d "$CAP" ]] || { echo "采集目录不存在: $CAP" >&2; exit 1; }
[[ -z "$NAME" ]] && NAME="real_$(basename "$CAP")"
INIT="$AI_TRAIN/init_models/helena_pusch${PRB}_sm_hi"
BUCKET=(); [[ "$PRB" == "106" ]] && BUCKET=(--bucket 106)
NSC=$((PRB * 12))

echo "== [1/5] 配对（slot 精确 + 候选）=="
"$VENV_PY" "$AI_TRAIN/pair_capture.py" "$CAP" --out "$CAP/pairs_v2.npz"

echo "== [2/5] 标签重建（候选内容验证，门 -10）=="
"$VENV_PY" "$AI_TRAIN/build_labels.py" "$CAP" --pairs "$CAP/pairs_v2.npz" \
  --out "$CAP/labels_all.npz" "${BUCKET[@]}"

echo "== [3/5] 划分（后置门 $GATE dB）=="
"$VENV_PY" "$AI_TRAIN/split_labels.py" "$CAP/labels_all.npz" "$CAP/realtrain.npz" \
  --gate "$GATE"

echo "== [4/5] 微调（--norm 0.15 镜像运行时尺度，init=$INIT, lr=$LR, epochs=$EPOCHS）=="
"$VENV_PY" "$AI_TRAIN/train_pad.py" "$CAP/realtrain.npz" \
  "$CAP/helena_pusch${PRB}_sm_${NAME}" \
  --prb "$PRB" --init "$INIT" --epochs "$EPOCHS" --batch 32 --lr "$LR" --norm 0.15

echo "== [5/5] 转换 + 入库 =="
"$VENV_PY" "$AI_TRAIN/convert_coreml.py" "$CAP/helena_pusch${PRB}_sm_${NAME}" \
  "$CAP/helena_pusch${PRB}_${NAME}.mlpackage" --shape "$NSC"
rm -rf "$CAP/mlc_out"
xcrun coremlcompiler compile "$CAP/helena_pusch${PRB}_${NAME}.mlpackage" "$CAP/mlc_out"
DEST="$ASSETS/helena_pusch${PRB}_${NAME}_ml.mlmodelc"
rm -rf "$DEST"
cp -R "$CAP/mlc_out/helena_pusch${PRB}_${NAME}.mlmodelc" "$DEST"
echo
echo "模型已入库: $DEST"
echo
echo "== 使用方法（A/B 对照）=="
echo "gnb 默认 CE = cpu（classical）；用本模型跑 helena 腿:"
echo "  sudo ./build/apps/gnb/gnb -c configs/gnb_uhd_oaiue.yaml \\"
echo "    expert_phy --pusch_ldpc_decoder_type auto --pusch_channel_estimator_algo helena \\"
echo "    --pusch_channel_estimator_helena_model_path_$PRB $DEST"
echo "对照与判定: tools/run_compare.sh <本腿 gnb 日志>（SINR 可比性校验 + 按 rnti 分账）"
