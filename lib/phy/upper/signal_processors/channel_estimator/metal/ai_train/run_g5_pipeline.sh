#!/usr/bin/env bash
# G-5 离线管线 sidecar：采集目录 → 配对 → 标签重建 → 划分 → 微调 → 转换。
# 白天采数后一条命令走完夜间训练前半段；A/B 实机测试仍按手册 §8 手动执行。
#
# 用法:
#   run_g5_pipeline.sh <采集目录> [--prb 52|106] [--init <SavedModel>]
#                       [--epochs N] [--lr R] [--no-convert]
#
# 依赖: python3 venv（numpy + tensorflow + tf-keras；转换另需 macOS coremltools
#       + xcrun coremlcompiler）。venv 用 VENV_PY 指定，默认系统 python3。
# 自包含：脚本按自身所在目录找 ai_train 工具，不依赖 ~/ai_ce_work。
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

echo "== [3/5] train/val split (10% holdout, seed 42) =="
"$PY" - "$CAP" <<'EOF'
import numpy as np, sys
cap = sys.argv[1]
d = np.load(f'{cap}/labels_all.npz')
X, Y, W = d['X'], d['Y'], d['width']
rng = np.random.default_rng(42); perm = rng.permutation(len(W))
nva = max(len(W) // 10, 50); te, tr = perm[:nva], perm[nva:]
np.savez(f'{cap}/realtrain.npz', X_train=X[tr], Y_train=Y[tr], width_train=W[tr],
         X_test=X[te], Y_test=Y[te], width_test=W[te])
print(f'train={len(tr)} test={len(te)}')
EOF

echo "== [4/5] fine-tune (init=$INIT, lr=$LR, epochs=$EPOCHS) =="
"$PY" "$AI_TRAIN/train_pad.py" "$CAP/realtrain.npz" "$CAP/helena_pusch${PRB}_sm_real" \
  --prb "$PRB" --init "$INIT" --epochs "$EPOCHS" --batch 32 --lr "$LR"

echo "== [5/5] convert (macOS coremltools + coremlcompiler) =="
if [[ "$CONVERT" == "1" ]]; then
  "$PY" "$AI_TRAIN/convert_coreml.py" "$CAP/helena_pusch${PRB}_sm_real" \
    "$CAP/helena_pusch${PRB}_real.mlpackage" --shape $((PRB * 12))
  rm -rf "$CAP/mlc_out"
  xcrun coremlcompiler compile "$CAP/helena_pusch${PRB}_real.mlpackage" "$CAP/mlc_out"
  echo "modelc -> $CAP/mlc_out/helena_pusch${PRB}_real.mlmodelc"
  echo "（入库/上线按 AI_CE_G5_manual.md §7/§9 执行）"
else
  echo "skipped (--no-convert)"
fi
echo "PIPELINE-DONE"
