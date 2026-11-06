#!/usr/bin/env bash
# BUILD A GIVEN LIST OF LEGS AT FULL SIZE, ONE AFTER ANOTHER. Two or three of these run side by side.
#
# The limit is memory, not CPU: one leg's build holds seventeen gigabytes while it accumulates its samples, on a machine with sixty-four. Two workers fit,
# three would swap. The replay jobs are reduced so that two dumps do not oversubscribe the cores.
set -u
TRAIN=/Users/jiachengwang/ai_depth3
TOOLS=/Users/jiachengwang/dev/ocudu/lib/phy/upper/ai_signal_processors/depth3/ai_train
LOG=$TRAIN/build_hq.log
for leg in "$@"; do
  if ls "$TRAIN/datasets_full_w2/${leg}"_*.npz > /dev/null 2>&1 && [ -s "$TRAIN/tb_$leg/index_self.csv" ]; then
    echo "have $leg" >> "$LOG"
    continue
  fi
  echo "---- $leg $(date +%T)" >> "$LOG"
  TRAIN=$TRAIN DATASETS_DIR=$TRAIN/datasets_full_w2 STATE=$TRAIN/bins_state_full.json \
  Q_SELF=100000000 Q_HARQ=100000000 JOBS=5 \
  nice -n 10 "$TOOLS/queue_build_datasets.sh" "$leg" >> "$LOG" 2>&1
  echo "     done $leg: $(ls "$TRAIN/datasets_full_w2/${leg}"_*.npz 2>/dev/null | wc -l) npz $(date +%T)" >> "$LOG"
done
echo "===== worker done $(date +%T)" >> "$LOG"
