#!/bin/bash
# LEVER D ON TOP OF THE BATCH-16 LLR-POSTFILTER ARM. Same recipe as the clean llrp_b16 run on 126 (batch 16, seed
# 20261105, 40 epochs) plus one training-only head that regresses the transmitted QAM symbol at every data RE.
#
# The comparison that matters is 107-symaux against 126-llrp_b16: same batch, same seed, same epochs, only the
# auxiliary objective differs. The head is dropped at export (the bundle is 464 KB, same as without it).
NV=$(ls -d "$HOME"/work/ai_phy_train/.venv/lib/python3.12/site-packages/nvidia/*/lib 2>/dev/null | tr "\n" ":")
export LD_LIBRARY_PATH="$NV${LD_LIBRARY_PATH:-}"
echo "=== llrp + sym-aux 40 epochs on 107 $(date +%F" "%T)"
cd "$HOME/work/ai_phy_train" || exit 1
exec .venv/bin/python -u "$HOME/work/ai_phy_train/lib/phy/upper/ai_signal_processors/depth3/ai_train/train_depth3.py" \
  --train "$HOME/work/ai_phy_train/data/datasets_train_w2v3c" \
  --val "$HOME/work/ai_phy_train/data/datasets_monitor_w2" \
  --out "$HOME/work/ai_phy_train/runs/d3_w2v3c_c32b18dense_e2e-cond-norep-sw2s3-llrp-symaux_e40" \
  --modulation QPSK --modcond --no-reporting-heads --c-hid 32 --blocks 18 --layout spatial \
  --dense-blocks --llr-post --sym-aux --w-sym 0.1 --scale-init 0.03 --snr-weight 2 --snr-sigma 3 --loss-norm bits --smooth-k 0 \
  --epochs 40 --patience 0 --snapshot-every 1 --batch 16 --workers 6 --max-val 1000 \
  --bler-samples 0 --seed 20261105 --lr 1e-3 --jit
