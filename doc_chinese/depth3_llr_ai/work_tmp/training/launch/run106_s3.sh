#!/bin/bash
# THIRD SEED (20261107), identical to s1/s2 in every other respect: w2v3c + c32 dense + sw2s3 + XLA, 40 epochs,
# constant learning rate, batch 8 (sharing the 8 GB card with the constellation-head arm).
#
# WHY A THIRD SEED. s1 and s2 gave 11 592 976 and 11 558 960 at their best checkpoints, and a tail average of s1 gave
# 11 651 632. One seed cannot tell a recipe from a draw; a third run makes the seed spread measurable and gives the
# tail-averaging method an independent test on a model it has never seen.
NV=$(ls -d "$HOME"/work/ai_phy_train/.venv/lib/python3.12/site-packages/nvidia/*/lib 2>/dev/null | tr "\n" ":")
export LD_LIBRARY_PATH="$NV${LD_LIBRARY_PATH:-}"
echo "=== s20261107 40 epochs on 106 $(date +%F" "%T)"
cd "$HOME/work/ai_phy_train" || exit 1
exec .venv/bin/python -u "$HOME/work/ai_phy_train/lib/phy/upper/ai_signal_processors/depth3/ai_train/train_depth3.py" \
  --train "$HOME/work/ai_phy_train/data/datasets_train_w2v3c" \
  --val "$HOME/work/ai_phy_train/data/datasets_monitor_w2" \
  --out "$HOME/work/ai_phy_train/runs/d3_w2v3c_c32b18dense_e2e-cond-norep-sw2s3_s20261107_e40" \
  --modulation QPSK --modcond --no-reporting-heads --c-hid 32 --blocks 18 --layout spatial \
  --dense-blocks --scale-init 0.03 --snr-weight 2 --snr-sigma 3 --loss-norm bits --smooth-k 0 \
  --epochs 40 --patience 0 --snapshot-every 1 --batch 8 --workers 8 --max-val 1000 \
  --bler-samples 0 --seed 20261107 --lr 1e-3 --jit
