#!/bin/bash
# LEVER D ALONE, on the good baseline: --sym-aux with NO llrp and NO decay, batch 16, seed 20261105, 40 epochs.
#
# WHY. The only measurement of the symbol head so far sat on top of llrp, which has now lost on both widths (-1.58 pp
# at c32, -0.88 pp at c128). Its +0.70/+1.14 pp advantage over that control therefore says nothing about the good
# baseline. This run puts the same head on the s1 recipe, so the comparison is s1's tail EMA (+1.79% is the decay
# variant; s1 itself is +0.75%) against its own tail EMA with the head.
#
# It also uses the new held-out monitor (1102a, 108 stratified samples, zero overlap with training, probe or ruler) and
# the selection-free mode: no per-epoch val, a coded-BLER health check every 5 epochs, no val-based 'best' checkpoint.
NV=$(ls -d "$HOME"/work/ai_phy_train/.venv/lib/python3.12/site-packages/nvidia/*/lib 2>/dev/null | tr "\n" ":")
export LD_LIBRARY_PATH="$NV${LD_LIBRARY_PATH:-}"
echo "=== symaux alone 40 epochs on 106 $(date +%F" "%T)"
cd "$HOME/work/ai_phy_train" || exit 1
exec .venv/bin/python -u "$HOME/work/ai_phy_train/lib/phy/upper/ai_signal_processors/depth3/ai_train/train_depth3.py" \
  --train "$HOME/work/ai_phy_train/data/datasets_train_w2v3c" \
  --val "$HOME/work/ai_phy_train/data/datasets_monitor_1102a" \
  --out "$HOME/work/ai_phy_train/runs/d3_w2v3c_c32b18dense_e2e-cond-norep-sw2s3-symaux_e40" \
  --modulation QPSK --modcond --no-reporting-heads --c-hid 32 --blocks 18 --layout spatial \
  --dense-blocks --sym-aux --w-sym 0.1 --scale-init 0.03 --snr-weight 2 --snr-sigma 3 --loss-norm bits --smooth-k 0 \
  --epochs 40 --snapshot-every 1 --batch 16 --workers 6 --max-val 1000 \
  --no-val --monitor-every 5 --bler-samples 24 --bler-every 5 --select-on none \
  --bler-samples 24 --seed 20261105 --lr 1e-3
