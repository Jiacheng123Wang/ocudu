#!/bin/bash
# THE DECAY CONTINUATION, SECOND SEED. s2's decayed continuation is the largest single lever measured (+1.04 pp over
# its own undecayed tail: -0.08% -> +1.77% on the ruler), but it rests on ONE seed. This repeats the identical recipe on
# s3, whose checkpoints already live on this machine, so no cross-machine copy is needed.
#
# THE RECIPE, copied from run107_decay.sh: a NEW output directory is seeded with the parent's last weights and its
# resume state, and the run resumes from there with a cosine decay that spans THIS continuation (--lr-decay-from 41),
# not the whole 1..80. Decaying the global schedule instead would have started the continuation at a half-decayed rate.
set -u
PARENT="$HOME/work/ai_phy_train/runs/d3_w2v3c_c32b18dense_e2e-cond-norep-sw2s3_s20261107_e40"
OUT="$HOME/work/ai_phy_train/runs/d3_w2v3c_c32b18dense_e2e-cond-norep-sw2s3_s20261107_e40_decay40"
mkdir -p "$OUT"
[ -f "$OUT/depth3_last.keras" ] || cp "$PARENT/depth3_last.keras" "$OUT/depth3_last.keras"
[ -f "$OUT/resume.json" ] || cp "$PARENT/resume.json" "$OUT/resume.json"
NV=$(ls -d "$HOME"/work/ai_phy_train/.venv/lib/python3.12/site-packages/nvidia/*/lib 2>/dev/null | tr "\n" ":")
export LD_LIBRARY_PATH="$NV${LD_LIBRARY_PATH:-}"
echo "=== s3 e40 -> e80 with cosine decay $(date +%F" "%T)"
cd "$HOME/work/ai_phy_train" || exit 1
exec .venv/bin/python -u "$HOME/work/ai_phy_train/lib/phy/upper/ai_signal_processors/depth3/ai_train/train_depth3.py" \
  --train "$HOME/work/ai_phy_train/data/datasets_train_w2v3c" \
  --val "$HOME/work/ai_phy_train/data/datasets_monitor_1102a" \
  --out "$OUT" \
  --modulation QPSK --modcond --no-reporting-heads --c-hid 32 --blocks 18 --layout spatial \
  --dense-blocks --scale-init 0.03 --snr-weight 2 --snr-sigma 3 --loss-norm bits --smooth-k 0 \
  --epochs 80 --lr-decay cosine --lr-min 1e-4 --lr-decay-from 41 \
  --snapshot-every 1 --batch 16 --workers 2 --max-val 1000 \
  --no-val --monitor-every 10 --bler-samples 0 --select-on none \
  --seed 20261107 --lr 1e-3 --resume
