#!/usr/bin/env bash
# BUILD THE LEGS NAMED IN A LIST FILE AT FULL SIZE INTO data/datasets_full_w2, RESTORING CAPTURES FROM THE ARCHIVE AS NEEDED.
#
# The list exists because the machine must not build what the other one builds: the sixty-four and two-hundred-and-fifty-six QAM legs are being built where
# their captures are, and their arrays will arrive here as a tarball. Building them here too would be wasted hours and two copies of the same data.
set -u
TRAIN=$HOME/work/ai_phy_train
TOOLS=$HOME/work/ocudu/lib/phy/upper/ai_signal_processors/depth3/ai_train
DEST=$TRAIN/data/datasets_full_w2
CORPUS=$TRAIN/corpus
TARS=/mnt/corpus/tarballs
LOG=$TRAIN/logs/build_full.log
LIST=${1:?usage: build_from_list.sh <list file>}

while read -r leg; do
  [ -n "$leg" ] || continue
  [ -f "$DEST/.done_$leg" ] && { echo "have $leg" >> "$LOG"; continue; }

  if [ -d "$CORPUS/$leg" ]; then
    local_corpus=1
  else
    local_corpus=0
    [ -f "$TARS/$leg.tar" ] || { echo "NO TAR $leg" >> "$LOG"; continue; }
    echo "---- restore $leg $(date +%T)" >> "$LOG"
    tar -xf "$TARS/$leg.tar" -C "$CORPUS" 2>/dev/null || { echo "extract failed $leg" >> "$LOG"; continue; }
  fi

  echo "---- build $leg (local=$local_corpus) $(date +%T)" >> "$LOG"
  if TRAIN=$TRAIN DATASETS_DIR=$DEST STATE=$TRAIN/bins_state_full.json \
     Q_SELF=100000000 Q_HARQ=100000000 JOBS=12 \
     "$TOOLS/queue_build_datasets.sh" "$leg" >> "$LOG" 2>&1; then
    touch "$DEST/.done_$leg"
    echo "     done $leg: $(ls "$DEST/${leg}"_*.npz 2>/dev/null | wc -l) npz $(date +%T)" >> "$LOG"
  else
    echo "     FAILED $leg $(date +%T)" >> "$LOG"
  fi
  [ "$local_corpus" = 0 ] && rm -rf "$CORPUS/$leg"
done < "$LIST"
echo "===== list done $(date +%T)" >> "$LOG"
