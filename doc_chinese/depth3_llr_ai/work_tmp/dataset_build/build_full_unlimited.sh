#!/usr/bin/env bash
# BUILD EVERY LEG AT FULL SIZE INTO data/datasets_full_w2, RESTORING EACH LEG'S CAPTURES FROM THE ARCHIVE FIRST.
#
# The mother set is unlimited by definition, so the quota is set absurdly high and the ledger is its own file: the sampled build's accounting must not
# constrain it, and it must not touch that build's state either.
#
# The captures have already been archived and deleted, so each leg is restored from its tar, built, and removed again -- one leg in flight at a time, so
# the transient cost is one leg's capture set rather than fifty.
set -u
TRAIN=$HOME/work/ai_phy_train
TOOLS=$HOME/work/ocudu/lib/phy/upper/ai_signal_processors/depth3/ai_train
DEST=$TRAIN/data/datasets_full_w2
CORPUS=$TRAIN/corpus
TARS=/mnt/corpus/tarballs
LOG=$TRAIN/logs/build_full.log
mkdir -p "$DEST" "$CORPUS"

for d in "$TRAIN"/tb_w2_harq_*/; do
  leg=$(basename "$d"); leg=${leg#tb_}
  [ -s "$d/index.csv" ] || continue
  if ls "$DEST"/${leg}_*.npz > /dev/null 2>&1; then
    echo "have $leg" >> "$LOG"
    continue
  fi
  tar_file=$TARS/$leg.tar
  [ -f "$tar_file" ] || { echo "NO TAR $leg" >> "$LOG"; continue; }
  echo "---- $leg $(date +%T)" >> "$LOG"
  tar -xf "$tar_file" -C "$CORPUS" 2>/dev/null || { echo "extract failed $leg" >> "$LOG"; continue; }
  DATASETS_DIR=$DEST STATE=$TRAIN/bins_state_full.json Q_SELF=100000000 Q_HARQ=100000000 \
    TRAIN=$TRAIN "$TOOLS/queue_build_datasets.sh" "$leg" >> "$LOG" 2>&1
  rm -rf "$CORPUS/$leg"
  echo "     built $(ls "$DEST"/${leg}_*.npz 2>/dev/null | wc -l) files, corpus removed $(date +%T)" >> "$LOG"
done
echo "===== all done $(date +%T)" >> "$LOG"
