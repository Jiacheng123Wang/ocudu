#!/bin/bash
# Roll the hard-sample harvest out over many legs: harvest, build every (modulation, population), audit, publish.
#
# THE NAMES CARRY THE LEG. The mother set's files are `<leg>_<MOD>_<population>.npz`, and a file called `self_hard.npz`
# cannot be traced back to a capture leg; the builder's `--name` is what puts the leg back into the file name.
#
# THE AUDIT RUNS BEFORE THE UPLOAD. A dataset that fails `check_depth3_dataset.py` is worth less than no dataset, because
# it will be trained on; so a leg that does not come out `findings: none` is left in place and reported, never published.
#
# Usage: rollout_hard.sh <leg list file> <work root> [--jobs 12] [--cap 20] [--kind amc]
set -u
LIST=$1; WORK=$2; shift 2
JOBS=12; CAP=20; KIND=amc
while [ $# -gt 0 ]; do case "$1" in --jobs) JOBS=$2; shift 2;; --cap) CAP=$2; shift 2;; --kind) KIND=$2; shift 2;; *) exit 2;; esac; done
HERE=$(cd "$(dirname "$0")" && pwd)
AI=$HOME/work/ai_phy_train
SRC=192.168.31.211
mkdir -p "$WORK"

while read -r LEG; do
  [ -n "$LEG" ] || continue
  echo "=== $LEG $(date +%H:%M:%S)"
  if [ ! -f "$WORK/$LEG/out/index_cap20.csv" ]; then
    bash "$HERE/harvest_leg_chain.sh" "$LEG" "$KIND" "$WORK/$LEG" --jobs "$JOBS" --cap "$CAP" || { echo "  harvest failed"; continue; }
  fi
  for MOD in QPSK 16QAM 64QAM 256QAM; do
    for POP in self harq; do
      NAME="${LEG}_${MOD}_${POP}_hard"
      OUTD="$WORK/$LEG/npz"
      mkdir -p "$OUTD"
      [ -s "$OUTD/$NAME.npz" ] && continue
      python3 "$AI/lib/phy/upper/ai_signal_processors/depth3/ai_train/build_depth3_dataset.py" \
        --units "$WORK/$LEG/out/index_${POP}_hard.csv" --tb "$WORK/$LEG/out/tb" \
        --corpus "$WORK/$LEG/$LEG" --out "$OUTD" --name "$NAME" \
        --modulation "$MOD" --min-prb 1 > "$WORK/$LEG/npz/$NAME.build.log" 2>&1
      if [ -s "$OUTD/$NAME.npz" ]; then
        FIND=$(python3 "$AI/lib/phy/upper/ai_signal_processors/depth3/ai_train/check_depth3_dataset.py" \
                 --dir "$OUTD/$NAME.npz" --full 2>&1 | sed -n 's/^ *\([a-z].*\)$/\1/p' | tail -1)
        echo "  $NAME: $(stat -c %s "$OUTD/$NAME.npz") B, audit ${FIND:-unknown}"
      else
        echo "  $NAME: not built (see build log)"
        rm -f "$OUTD/$NAME.npz"
      fi
    done
  done
  if [ -n "$(ls -A "$WORK/$LEG/npz" 2>/dev/null | grep '\.npz$')" ]; then
    ssh -o BatchMode=yes jwang@$SRC "mkdir -p /mnt/corpus/harvest_hard/$LEG/npz"
    scp -q "$WORK/$LEG/npz/"*.npz jwang@$SRC:/mnt/corpus/harvest_hard/$LEG/npz/ && echo "  published $(ls "$WORK/$LEG/npz"/*.npz | wc -l) npz"
  fi
done < "$LIST"
echo ROLLOUT_DONE
