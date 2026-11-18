#!/bin/bash
# Resume the c128 + w2v3c + sw2s3 run from e23 to 40 (stopped for the move with 22 epochs done).
NV=$(ls -d "$HOME"/work/ai_phy_train/.venv216/lib/python3.12/site-packages/nvidia/*/lib 2>/dev/null | tr "\n" ":")
export LD_LIBRARY_PATH="$NV${LD_LIBRARY_PATH:-}"
echo "=== resume c128 to e40 $(date +%F" "%T)"
cd "$HOME/work/ai_phy_train" || exit 1
exec .venv216/bin/python -u "$HOME/work/ai_phy_train/lib/phy/upper/ai_signal_processors/depth3/ai_train/train_depth3.py" \
  --train "$HOME/work/ai_phy_train/data/datasets_train_w2v3c" \
  --val "$HOME/work/ai_phy_train/data/datasets_monitor_w2" \
  --out "$HOME/work/ai_phy_train/runs/d3_w2v3c_c128b18dense_sc003_sw2s3_e10" \
  --modulation QPSK --modcond --no-reporting-heads --c-hid 128 --blocks 18 --layout spatial \
  --dense-blocks --scale-init 0.03 --snr-weight 2 --snr-sigma 3 --loss-norm bits --smooth-k 0 \
  --epochs 40 --patience 0 --snapshot-every 1 --batch 8 --workers 6 --max-val 1000 \
  --bler-samples 0 --seed 20261105 --lr 1e-3 --jit --resume
