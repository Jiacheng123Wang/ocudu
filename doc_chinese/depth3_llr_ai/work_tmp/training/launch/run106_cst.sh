#!/bin/bash
# LEVER A: constellation-aware head (--cst-head): analytic 4*s/sigma^2 term plus a learned residual, +99 parameters.
#
# RUNS ALONGSIDE LEVER B. 106 turned out to be latency-bound per step rather than saturated (GPU 64% / 200 W, disk
# 16%, one and a half cores of sixteen), so two jobs at batch 8 share the box better than one at batch 16 and both
# results land in the same night.
NV=$(ls -d "$HOME"/work/ai_phy_train/.venv/lib/python3.12/site-packages/nvidia/*/lib 2>/dev/null | tr "\n" ":")
export LD_LIBRARY_PATH="$NV${LD_LIBRARY_PATH:-}"
echo "=== cst 40 epochs on 106 $(date +%F" "%T)"
cd "$HOME/work/ai_phy_train" || exit 1
exec .venv/bin/python -u "$HOME/work/ai_phy_train/lib/phy/upper/ai_signal_processors/depth3/ai_train/train_depth3.py" \
  --train "$HOME/work/ai_phy_train/data/datasets_train_w2v3c" \
  --val "$HOME/work/ai_phy_train/data/datasets_monitor_w2" \
  --out "$HOME/work/ai_phy_train/runs/d3_w2v3c_c32b18dense_e2e-cond-norep-sw2s3-cst_e40" \
  --modulation QPSK --modcond --no-reporting-heads --c-hid 32 --blocks 18 --layout spatial \
  --dense-blocks --cst-head --scale-init 0.03 --snr-weight 2 --snr-sigma 3 --loss-norm bits --smooth-k 0 \
  --epochs 40 --patience 0 --snapshot-every 1 --batch 8 --workers 8 --max-val 1000 \
  --bler-samples 0 --seed 20261105 --lr 1e-3 --jit
