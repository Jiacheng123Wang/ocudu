#!/usr/bin/env bash
# BUILD EVERY LEG AT FULL SIZE INTO data/datasets_full_w2 ON THIS MACHINE, RESTORING CAPTURES FROM THE ARCHIVE ONLY WHEN THEY ARE NOT HERE.
#
# The mother set is unlimited by definition, so the quota is absurdly high and the ledger is its own file: the sampled build's accounting must not constrain
# it, and it must not touch that build's state either.
#
# Two things this fixes from its first version. It no longer trusts "an output file exists" as proof a leg is done -- a build interrupted half way leaves one
# of four files -- it writes a stamp only after the queue returns success. And it only unpacks a leg's captures when they are not already in corpus/.
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
  [ -f "$DEST/.done_$leg" ] && { echo "have $leg" >> "$LOG"; continue; }

  if [ -d "$CORPUS/$leg" ]; then
    local_corpus=1
  else
    local_corpus=0
    [ -f "$TARS/$leg.tar" ] || { echo "NO TAR $leg" >> "$LOG"; continue; }
    echo "---- restore $leg $(date +%T)" >> "$LOG"
    tar -xf "$TARS/$leg.tar" -C "$CORPUS" 2>/dev/null || { echo "extract failed $leg" >> "$LOG"; continue; }
  fi

  echo "---- build $leg (corpus local=$local_corpus) $(date +%T)" >> "$LOG"
  if TRAIN=$TRAIN DATASETS_DIR=$DEST STATE=$TRAIN/bins_state_full.json \
     Q_SELF=100000000 Q_HARQ=100000000 JOBS=12 \
     "$TOOLS/queue_build_datasets.sh" "$leg" >> "$LOG" 2>&1; then
    touch "$DEST/.done_$leg"
    echo "     done $leg: $(ls "$DEST/${leg}"_*.npz 2>/dev/null | wc -l) npz $(date +%T)" >> "$LOG"
  else
    echo "     FAILED $leg $(date +%T)" >> "$LOG"
  fi
  [ "$local_corpus" = 0 ] && rm -rf "$CORPUS/$leg"
done
echo "===== all done $(date +%T)" >> "$LOG"
