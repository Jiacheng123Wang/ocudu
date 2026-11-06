#!/usr/bin/env bash
# BUILD A LIST OF LEGS AT FULL SIZE, AND REMEMBER WHICH ONES ACTUALLY FINISHED.
#
# The earlier version skipped a leg if ANY output file existed, which mistakes a build that was interrupted half way -- one file written, three missing --
# for a finished one. A leg is now marked done by a stamp written only after the queue returns success, so an interrupted leg is retried rather than
# silently left incomplete.
set -u
TRAIN=/Users/jiachengwang/ai_depth3
TOOLS=/Users/jiachengwang/dev/ocudu/lib/phy/upper/ai_signal_processors/depth3/ai_train
LOG=$TRAIN/build_hq.log
STAMP=$TRAIN/datasets_full_w2

for leg in "$@"; do
  if [ -f "$STAMP/.done_$leg" ]; then
    echo "have $leg" >> "$LOG"
    continue
  fi
  echo "---- $leg $(date +%T)" >> "$LOG"
  if TRAIN=$TRAIN DATASETS_DIR=$TRAIN/datasets_full_w2 STATE=$TRAIN/bins_state_full.json \
     Q_SELF=100000000 Q_HARQ=100000000 JOBS=5 \
     nice -n 10 "$TOOLS/queue_build_datasets.sh" "$leg" >> "$LOG" 2>&1; then
    touch "$STAMP/.done_$leg"
    echo "     done $leg: $(ls "$STAMP/${leg}"_*.npz 2>/dev/null | wc -l) npz $(date +%T)" >> "$LOG"
  else
    echo "     FAILED $leg $(date +%T)" >> "$LOG"
  fi
done
echo "===== worker done $(date +%T)" >> "$LOG"
