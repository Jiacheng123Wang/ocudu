#!/bin/bash
# LEVER A + LEVER B: the pointwise expansion (--mlp-expand 2) TOGETHER WITH squeeze-excite channel attention.
#
# WHY THESE TWO. A mixes the channels twice per residual and measured +0.38 pp over s1 on the ruler (11 696 424 vs
# 11 651 632). B re-weights the channels from a global summary instead of mixing them more, so it is orthogonal and
# costs about 5k parameters. This run is the single-variable test of B on top of A: same data, batch, seed, epochs.
NV=$(ls -d "$HOME"/work/ai_phy_train/.venv/lib/python3.12/site-packages/nvidia/*/lib 2>/dev/null | tr "\n" ":")
export LD_LIBRARY_PATH="$NV${LD_LIBRARY_PATH:-}"
echo "=== mlp2 + se 40 epochs on 107 $(date +%F" "%T)"
cd "$HOME/work/ai_phy_train" || exit 1
exec .venv/bin/python -u "$HOME/work/ai_phy_train/lib/phy/upper/ai_signal_processors/depth3/ai_train/train_depth3.py" \
  --train "$HOME/work/ai_phy_train/data/datasets_train_w2v3c" \
  --val "$HOME/work/ai_phy_train/data/datasets_monitor_1102a" \
  --out "$HOME/work/ai_phy_train/runs/d3_w2v3c_c32b18dense_e2e-cond-norep-sw2s3-mlp2-se_e40" \
  --modulation QPSK --modcond --no-reporting-heads --c-hid 32 --blocks 18 --layout spatial \
  --dense-blocks --mlp-expand 2 --se --se-ratio 8 --scale-init 0.03 --snr-weight 2 --snr-sigma 3 \
  --loss-norm bits --smooth-k 0 \
  --epochs 40 --snapshot-every 1 --batch 16 --workers 6 --max-val 1000 \
  --no-val --monitor-every 5 --bler-samples 0 --bler-every 5 --select-on none \
  --resume --seed 20261105 --lr 1e-3
