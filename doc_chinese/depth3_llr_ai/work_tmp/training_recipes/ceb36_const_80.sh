#!/bin/bash
# Ce+B at THIRTY-SIX blocks, constant rate to eighty epochs  (the current best-recipe candidate)
#
# WHY. Three levers have each been measured on their own and none of them has been stacked with the others: depth
# (18 -> 36 blocks, +0.52 pp at forty epochs), B (channel attention, +0.38 pp at eighty epochs), and length (40 -> 80
# epochs at a constant rate, +0.67 pp). The best model so far is Ce+B decayed to eighty epochs at 11,937,280 (+3.22%).
# This run keeps the rate constant, so the forty-epoch tail can be measured against the Ce+36 control (11,875,808) and
# the eighty-epoch tail against the decayed eighteen-block model -- both comparisons share one trajectory.
set -u
NV=$(ls -d "$HOME"/work/ai_phy_train/.venv/lib/python3.12/site-packages/nvidia/*/lib 2>/dev/null | tr "\n" ":")
export LD_LIBRARY_PATH="$NV${LD_LIBRARY_PATH:-}"
export TF_GPU_ALLOCATOR=cuda_malloc_async
echo "=== dense-skip-entry + se, 36 blocks, constant lr, epochs 1..80 $(date +%F" "%T)"
cd "$HOME/work/ai_phy_train" || exit 1
exec .venv/bin/python -u "$HOME/work/ai_phy_train/lib/phy/upper/ai_signal_processors/depth3/ai_train/train_depth3.py" \
  --train "$HOME/work/ai_phy_train/data/datasets_train_w2v3c" \
  --val "$HOME/work/ai_phy_train/data/datasets_monitor_1102a" \
  --out "$HOME/work/ai_phy_train/runs/d3_w2v3c_c32b36dense_e2e-cond-norep-sw2s3-dskip-entry-se_e80" \
  --modulation QPSK --modcond --no-reporting-heads --c-hid 32 --blocks 36 --layout spatial \
  --dense-blocks --dense-skip-entry --se \
  --scale-init 0.03 --snr-weight 2 --snr-sigma 3 --loss-norm bits --smooth-k 0 \
  --epochs 80 --snapshot-every 1 --batch 16 --workers 1 --max-val 1000 \
  --no-val --monitor-every 10 --bler-samples 0 --select-on none \
  --seed 20261105 --lr 1e-3
