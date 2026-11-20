#!/bin/bash
# CLEAN RE-RUN OF LEVER B AT BATCH 16. The first llrp arm was trained 6 epochs at batch 16 and 33 at batch 8 (the card
# had to share with the constellation head), so its +0.68% ruler result cannot be attributed to the LLR-plane filter
# alone. This run is identical except that it keeps batch 16 for all 40 epochs.
NV=$(ls -d "$HOME"/work/ai_phy_train/.venv216/lib/python3.12/site-packages/nvidia/*/lib 2>/dev/null | tr "\n" ":")
export LD_LIBRARY_PATH="$NV${LD_LIBRARY_PATH:-}"
echo "=== llrp batch16 clean 40 epochs on 126 $(date +%F" "%T)"
cd "$HOME/work/ai_phy_train" || exit 1
exec .venv216/bin/python -u "$HOME/work/ai_phy_train/lib/phy/upper/ai_signal_processors/depth3/ai_train/train_depth3.py" \
  --train "$HOME/work/ai_phy_train/data/datasets_train_w2v3c" \
  --val "$HOME/work/ai_phy_train/data/datasets_monitor_w2" \
  --out "$HOME/work/ai_phy_train/runs/d3_w2v3c_c32b18dense_e2e-cond-norep-sw2s3-llrp_b16_e40" \
  --modulation QPSK --modcond --no-reporting-heads --c-hid 32 --blocks 18 --layout spatial \
  --dense-blocks --llr-post --scale-init 0.03 --snr-weight 2 --snr-sigma 3 --loss-norm bits --smooth-k 0 \
  --epochs 40 --patience 0 --snapshot-every 1 --batch 16 --workers 6 --max-val 1000 \
  --bler-samples 0 --seed 20261105 --lr 1e-3 --jit
