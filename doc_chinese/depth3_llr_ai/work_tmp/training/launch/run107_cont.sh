#!/bin/bash
# CONTINUE THE seed-20261106 ARM FROM ITS RULER-BEST CHECKPOINT (e32), 40 MORE EPOCHS.
#
# Why from e32 and not e40: the delivered bits swing by four percentage points between checkpoints of the same run
# (e32 11 558 960 vs e40 11 096 072 on the ruler), and val does not predict which is which (e32 is val-1st, e40 8th).
# e32 is the best checkpoint this run produced, so the continuation starts there rather than from the last epoch.
#
# The run dir is pre-seeded with that checkpoint as `depth3_last.keras` and a `resume.json` whose epoch is 32, so the
# trainer resumes as if it had just finished epoch 32 and continues 33..72 without repeating any work.
NV=$(ls -d "$HOME"/work/ai_phy_train/.venv/lib/python3.12/site-packages/nvidia/*/lib 2>/dev/null | tr "\n" ":")
export LD_LIBRARY_PATH="$NV${LD_LIBRARY_PATH:-}"
echo "=== s20261106 continue from e32 $(date +%F" "%T)"
cd "$HOME/work/ai_phy_train" || exit 1
exec .venv/bin/python -u "$HOME/work/ai_phy_train/lib/phy/upper/ai_signal_processors/depth3/ai_train/train_depth3.py" \
  --train "$HOME/work/ai_phy_train/data/datasets_train_w2v3c" \
  --val "$HOME/work/ai_phy_train/data/datasets_monitor_w2" \
  --out "$HOME/work/ai_phy_train/runs/d3_w2v3c_c32b18dense_e2e-cond-norep-sw2s3_s20261106_e32cont" \
  --modulation QPSK --modcond --no-reporting-heads --c-hid 32 --blocks 18 --layout spatial \
  --dense-blocks --scale-init 0.03 --snr-weight 2 --snr-sigma 3 --loss-norm bits --smooth-k 0 \
  --epochs 72 --patience 0 --snapshot-every 1 --batch 16 --workers 6 --max-val 1000 \
  --bler-samples 0 --seed 20261106 --lr 1e-3 --jit --resume
