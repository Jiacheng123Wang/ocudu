#!/bin/bash
# CAPACITY + THE LLR-PLANE FILTER TOGETHER. Two levers are independently validated: c128 (a single checkpoint at +1.37%,
# its tail average at +1.66%) and the LLR-plane post-filter (its tail average at +1.09%). Every attempt to add
# *information* to the trunk failed (dilation, big kernel, noise feedback, channel head); every attempt to add
# capacity or to filter the answer itself has worked. This run puts the two working ones in one model.
#
# 40 epochs at batch 8 (c128 does not fit at 16 on an 8 GB card); seed 20261105 so it lines up with s1/s2/llrp.
NV=$(ls -d "$HOME"/work/ai_phy_train/.venv/lib/python3.12/site-packages/nvidia/*/lib 2>/dev/null | tr "\n" ":")
export LD_LIBRARY_PATH="$NV${LD_LIBRARY_PATH:-}"
echo "=== c128 + llrp 40 epochs on 106 $(date +%F" "%T)"
cd "$HOME/work/ai_phy_train" || exit 1
exec .venv/bin/python -u "$HOME/work/ai_phy_train/lib/phy/upper/ai_signal_processors/depth3/ai_train/train_depth3.py" \
  --train "$HOME/work/ai_phy_train/data/datasets_train_w2v3c" \
  --val "$HOME/work/ai_phy_train/data/datasets_monitor_w2" \
  --out "$HOME/work/ai_phy_train/runs/d3_w2v3c_c128b18dense_e2e-cond-norep-sw2s3-llrp_e40" \
  --modulation QPSK --modcond --no-reporting-heads --c-hid 128 --blocks 18 --layout spatial \
  --dense-blocks --llr-post --scale-init 0.03 --snr-weight 2 --snr-sigma 3 --loss-norm bits --smooth-k 0 \
  --epochs 40 --patience 0 --snapshot-every 1 --batch 8 --workers 6 --max-val 1000 \
  --bler-samples 0 --seed 20261105 --lr 1e-3 --jit
