#!/bin/bash
# The canonical training command lines, parameterised. Each is the one whose result is recorded in the PK; changing a
# flag here without a new PK row makes the numbers unattributable.
#
# Usage:  train_recipes.sh <recipe> [extra args...]
#   recipes: c128-sw2s3     clean w2v3c, c128 dense + sw2s3, 20 epochs (resume-capable; 66 min/epoch on a 3060 Ti)
#            c32-xla         clean w2v3c, c32 dense + sw2s3, XLA, 40 epochs (the current best, PK -0.65%)
#            c32-k5          c32 dense + sw2s3 + 5x5 kernel, XLA, 40 epochs (architecture lever L2)
#            c32-g4          c32 dense groups=4 + sw2s3, no XLA, 40 epochs (architecture lever L3)
#            c32-cliff       c32 dense + cliff-band weighting (centre 25.5 dB), no XLA, 40 epochs
set -u
R=${1:?recipe}; shift || true
TRAIN=${TRAIN:-$HOME/work/ai_phy_train}
DATA=${DATA:-$TRAIN/data/datasets_train_w2v3c}
VAL=${VAL:-$TRAIN/data/datasets_monitor_w2}
PY=${PY:-$TRAIN/.venv/bin/python}
CODE=${CODE:-$TRAIN/lib/phy/upper/ai_signal_processors/depth3/ai_train/train_depth3.py}
COMMON="--train $DATA --val $VAL --modulation QPSK --modcond --no-reporting-heads --layout spatial \
  --dense-blocks --scale-init 0.03 --loss-norm bits --smooth-k 0 --patience 0 --snapshot-every 1 \
  --batch 16 --workers 6 --max-val 1000 --bler-samples 0 --seed 20261105 --lr 1e-3"
case "$R" in
  c128-sw2s3) exec "$PY" -u "$CODE" $COMMON --c-hid 128 --blocks 18 --batch 8 --epochs 20 \
      --snr-weight 2 --snr-sigma 3 --out "$TRAIN/runs/d3_w2v3c_c128b18dense_sc003_sw2s3_e20" "$@" ;;
  c32-xla)    exec "$PY" -u "$CODE" $COMMON --c-hid 32 --blocks 18 --epochs 40 \
      --snr-weight 2 --snr-sigma 3 --jit --out "$TRAIN/runs/d3_w2v3c_c32b18dense_sc003_sw2s3_jit_e40" "$@" ;;
  c32-k5)     exec "$PY" -u "$CODE" $COMMON --c-hid 32 --blocks 18 --epochs 40 --kernel-size 5 \
      --snr-weight 2 --snr-sigma 3 --jit --out "$TRAIN/runs/d3_w2v3c_c32b18_e2e-cond-norep-sw2s3-k5_e40" "$@" ;;
  c32-g4)     exec "$PY" -u "$CODE" $COMMON --c-hid 32 --blocks 18 --epochs 40 --groups 4 \
      --snr-weight 2 --snr-sigma 3 --out "$TRAIN/runs/d3_w2v3c_c32b18_e2e-cond-norep-sw2s3-g4_e40" "$@" ;;
  c32-cliff)  exec "$PY" -u "$CODE" $COMMON --c-hid 32 --blocks 18 --epochs 40 \
      --snr-weight 6 --snr-sigma 1.5 --snr-center 25.5 \
      --out "$TRAIN/runs/d3_w2v3c_c32b18dense_sc003_cliff_e40" "$@" ;;
  *) echo "unknown recipe: $R" >&2; exit 2 ;;
esac
