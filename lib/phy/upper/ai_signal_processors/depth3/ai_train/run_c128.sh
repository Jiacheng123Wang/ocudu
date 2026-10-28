#!/usr/bin/env bash
# c128 on the whole e17 training set, with early stopping and automatic resume.
#
# RESTARTING IS NOT RESTARTING. Every epoch writes depth3_last.keras (weights plus optimizer) and resume.json, so a crash
# costs at most one epoch; this loop simply starts the trainer again with --resume, and stops when the trainer exits zero
# (which is what an early stop does).
set -u
cd "$HOME/work/ai_phy_train"
PY=./.venv/bin/python
TRAIN=lib/phy/upper/ai_signal_processors/depth3/ai_train/train_depth3.py
OUT=runs/c128_e17
mkdir -p runs
while true; do
  "$PY" -u "$TRAIN" \
    --train data/e17_train/depth3.npz --val data/e17_monitor/depth3.npz \
    --out "$OUT" --modcond --layout spatial --blocks 18 --c-hid 128 --in-channels 12 \
    --no-reporting-heads --w-ch 0.1 --w-noise 0.3 --batch 8 --epochs 60 \
    --patience 3 --bler-every 5 --bler-samples 8 --resume >> runs_c128.log 2>&1
  rc=$?
  echo "trainer exited rc=$rc at $(date +%T)" >> runs_c128.log
  [ "$rc" -eq 0 ] && break
  sleep 30
done
echo "c128 finished at $(date +%T)" >> runs_c128.log
