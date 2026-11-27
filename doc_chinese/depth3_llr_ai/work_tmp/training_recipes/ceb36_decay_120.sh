#!/bin/bash
# Ce+B at THIRTY-SIX blocks, e81..e120 with a COSINE learning-rate decay (1e-3 -> 1e-4).
#
# WHY THIS RUN. The parent is
#   runs/d3_w2v3c_c32b36dense_e2e-cond-norep-sw2s3-dskip-entry_e80
# trained on 211 with a CONSTANT rate to eighty epochs (argv in logs/ce36_e80_hk.log; 40 037 s for e56..e80,
# ~27 min/epoch). This script continues that trajectory from its epoch-80 weights to epoch 120 and changes ONE
# thing: the rate follows a cosine from 1e-3 at e81 down to 1e-4 at e120. Data, structure, loss weights, batch,
# workers, monitoring cadence and seed are copied verbatim from the parent argv, so the only difference between
# the two runs is the extra forty epochs plus the decay schedule.
#
# WHY DECAY. Measured on the same trajectories: a constant rate from 80 to 120 buys +0.03 pp (it just wanders),
# while the same forty epochs with cosine decay buy +0.38 pp (Ce) and +0.57..0.68 pp (Ce+B); the decayed tail is
# also flat across windows (0.04 pp versus 0.7 pp for a constant rate). See architecture_levers_beyond_width.md
# sections 94, 99, 100 and 101. The schedule mirrors the reference run on 107,
#   runs/d3_w2v3c_c32b36dense_e2e-cond-norep-sw2s3-dskip-entry-se_e120_decay
# i.e. --lr-decay cosine --lr-min 1e-4 --lr-decay-from 81 (that arm additionally has --se; this one does not).
#
# HOW THE RESUME WORKS. --resume reads <out>/depth3_last.keras + <out>/resume.json, so seed the output directory
# first:
#   SRC=runs/d3_w2v3c_c32b36dense_e2e-cond-norep-sw2s3-dskip-entry_e80
#   NEW=runs/d3_w2v3c_c32b36dense_e2e-cond-norep-sw2s3-dskip-entry_e120_decay
#   mkdir -p "$NEW" && cp -p "$SRC/depth3_last.keras" "$SRC/resume.json" "$NEW"/
# The epoch counter then continues at 81 and the schedule is computed from the epoch number (not a step counter).
# Expected banner: "next epoch 81, ... 80 earlier epoch(s) of history carried over", "parameters 485833",
# "lr 0.001 (cosine, from epoch 81)".
#
# LAUNCH (background, logs in logs/):
#   cd ~/work/ai_phy_train
#   setsid nohup bash run_211_ceb36_e120_decay.sh > logs/ceb36_e120_decay_211.log 2>&1 < /dev/null &
#
# ETA: 40 epochs x ~27 min = ~18 h on the 3060 Ti.
set -u
NV=$(ls -d "$HOME"/work/ai_phy_train/.venv/lib/python3.12/site-packages/nvidia/*/lib 2>/dev/null | tr "\n" ":")
export LD_LIBRARY_PATH="$NV${LD_LIBRARY_PATH:-}"
export TF_GPU_ALLOCATOR=cuda_malloc_async
echo "=== c32b36dense dskip-entry, cosine 1e-3->1e-4 from e81, e81..e120  $(date +%F" "%T)"
cd "$HOME/work/ai_phy_train" || exit 1
exec .venv/bin/python -u "$HOME/work/ai_phy_train/lib/phy/upper/ai_signal_processors/depth3/ai_train/train_depth3.py" \
  --train "$HOME/work/ai_phy_train/data/datasets_train_w2v3c" \
  --val "$HOME/work/ai_phy_train/data/datasets_train_w2v3c" \
  --out "$HOME/work/ai_phy_train/runs/d3_w2v3c_c32b36dense_e2e-cond-norep-sw2s3-dskip-entry_e120_decay" \
  --modulation QPSK --modcond --no-reporting-heads --c-hid 32 --blocks 36 --layout spatial \
  --dense-blocks --dense-skip-entry \
  --scale-init 0.03 --snr-weight 2 --snr-sigma 3 --loss-norm bits --smooth-k 0 \
  --epochs 120 --snapshot-every 1 --batch 16 --workers 6 --max-val 1000 \
  --no-val --monitor-every 5 --bler-samples 0 --select-on none \
  --seed 20261105 --lr 1e-3 \
  --lr-decay cosine --lr-min 1e-4 --lr-decay-from 81 \
  --resume
