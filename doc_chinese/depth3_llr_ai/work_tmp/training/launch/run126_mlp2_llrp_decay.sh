#!/bin/bash
# A + LLR-POST + COSINE DECAY in one run: the three levers that each measured a gain, stacked.
# The clean control for the A term is this same recipe without --mlp-expand 2 (the llrp_b16 run on this machine).
NV=$(ls -d "$HOME"/work/ai_phy_train/.venv216/lib/python3.12/site-packages/nvidia/*/lib 2>/dev/null | tr "\n" ":")
export LD_LIBRARY_PATH="$NV${LD_LIBRARY_PATH:-}"
echo "=== llrp + mlp2 + decay 40 epochs on 126 $(date +%F" "%T)"
cd "$HOME/work/ai_phy_train" || exit 1
exec .venv216/bin/python -u "$HOME/work/ai_phy_train/lib/phy/upper/ai_signal_processors/depth3/ai_train/train_depth3.py" \
  --train "$HOME/work/ai_phy_train/data/datasets_train_w2v3c" \
  --val "$HOME/work/ai_phy_train/data/datasets_monitor_w2" \
  --out "$HOME/work/ai_phy_train/runs/d3_w2v3c_c32b18dense_e2e-cond-norep-sw2s3-llrp-mlp2-decay_e40" \
  --modulation QPSK --modcond --no-reporting-heads --c-hid 32 --blocks 18 --layout spatial \
  --dense-blocks --llr-post --mlp-expand 2 --lr-decay cosine --lr-min 1e-4 --lr-decay-from 1 \
  --scale-init 0.03 --snr-weight 2 --snr-sigma 3 --loss-norm bits --smooth-k 0 \
  --epochs 40 --patience 0 --snapshot-every 1 --batch 16 --workers 6 --max-val 1000 \
  --bler-samples 0 --seed 20261105 --lr 1e-3
