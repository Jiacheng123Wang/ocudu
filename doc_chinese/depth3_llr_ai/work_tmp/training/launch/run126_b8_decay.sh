#!/bin/bash
# ARM A of the step-count test (architecture_levers section 36): batch 8 x 40 epochs = 918k steps, twice the update
# count of the batch-16 x 40 baseline, with the SAME data exposure. Cosine decay starts at epoch 30 (75% of the run),
# which is the schedule the one successful decay run used -- NOT from epoch 1, which halves the rate far too early.
#
# Compare against: s2 decay EMA (batch 16 x 40 + decay, ruler 11 772 720, +1.79%) and s1 tail SWA (11 651 632, +0.75%).
NV=$(ls -d "$HOME"/work/ai_phy_train/.venv216/lib/python3.12/site-packages/nvidia/*/lib 2>/dev/null | tr "\n" ":")
export LD_LIBRARY_PATH="$NV${LD_LIBRARY_PATH:-}"
echo "=== batch8-decay 40 epochs on 126 $(date +%F" "%T)"
cd "$HOME/work/ai_phy_train" || exit 1
exec .venv216/bin/python -u "$HOME/work/ai_phy_train/lib/phy/upper/ai_signal_processors/depth3/ai_train/train_depth3.py" \
  --train "$HOME/work/ai_phy_train/data/datasets_train_w2v3c" \
  --val "$HOME/work/ai_phy_train/data/datasets_monitor_1102a" \
  --out "$HOME/work/ai_phy_train/runs/d3_w2v3c_c32b18dense_e2e-cond-norep-sw2s3-b8-decay_e40" \
  --modulation QPSK --modcond --no-reporting-heads --c-hid 32 --blocks 18 --layout spatial \
  --dense-blocks --scale-init 0.03 --snr-weight 2 --snr-sigma 3 --loss-norm bits --smooth-k 0 \
  --lr-decay cosine --lr-min 1e-4 --lr-decay-from 30 \
  --epochs 40 --snapshot-every 1 --batch 8 --workers 6 --max-val 1000 \
  --no-val --monitor-every 5 --bler-samples 24 --bler-every 5 --select-on none \
  --seed 20261105 --lr 1e-3
