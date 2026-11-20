#!/bin/bash
# THE DECAY EXPERIMENT: continue seed-20261106 from e32 with a cosine decay 1e-3 -> 1e-4 across the continuation.
#
# WHY. With a constant rate the model keeps wandering in a ball, and consecutive checkpoints differ by up to 4.0 pp of
# delivered bits. A decayed end makes the last checkpoints close together, which is the condition the tail
# weight-averaging needs, and it is also the standard recipe for a settling phase.
#
# `--lr-decay-from 33` so the decay spans THIS continuation, not the whole 1..72 of the original run: the model was
# trained at 1e-3 up to e32, so the continuation has to start from 1e-3 rather than inherit a half-decayed position.
NV=$(ls -d "$HOME"/work/ai_phy_train/.venv/lib/python3.12/site-packages/nvidia/*/lib 2>/dev/null | tr "\n" ":")
export LD_LIBRARY_PATH="$NV${LD_LIBRARY_PATH:-}"
echo "=== s20261106 e32 -> e72 with cosine decay $(date +%F" "%T)"
cd "$HOME/work/ai_phy_train" || exit 1
exec .venv/bin/python -u "$HOME/work/ai_phy_train/lib/phy/upper/ai_signal_processors/depth3/ai_train/train_depth3.py" \
  --train "$HOME/work/ai_phy_train/data/datasets_train_w2v3c" \
  --val "$HOME/work/ai_phy_train/data/datasets_monitor_w2" \
  --out "$HOME/work/ai_phy_train/runs/d3_w2v3c_c32b18dense_e2e-cond-norep-sw2s3_s20261106_e32cont_decay" \
  --modulation QPSK --modcond --no-reporting-heads --c-hid 32 --blocks 18 --layout spatial \
  --dense-blocks --scale-init 0.03 --snr-weight 2 --snr-sigma 3 --loss-norm bits --smooth-k 0 \
  --epochs 72 --lr-decay cosine --lr-min 1e-4 --lr-decay-from 33 \
  --patience 0 --snapshot-every 1 --batch 16 --workers 6 --max-val 1000 \
  --bler-samples 0 --seed 20261106 --lr 1e-3 --jit --resume
