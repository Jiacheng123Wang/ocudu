#!/usr/bin/env bash
# BUILD THE LISTED LEGS AT FULL SIZE, TWO AT A TIME.
#
# The build is one process per leg, single threaded, holding about a gigabyte while it reads and fifteen by the time it assembles the arrays. The machine has
# sixteen threads and twenty-eight gigabytes available, so two fit comfortably and four threads of compute go unused either way. Serial was the previous
# version's mistake: it left fifteen cores idle for a day.
#
# Each leg gets its own stamp on success, so a rerun skips finished work and retries the rest.
set -u
TRAIN=$HOME/work/ai_phy_train
TOOLS=$HOME/work/ocudu/lib/phy/upper/ai_signal_processors/depth3/ai_train
DEST=$TRAIN/data/datasets_full_w2
CORPUS=$TRAIN/corpus
TARS=/mnt/corpus/tarballs
LOG=$TRAIN/logs/build_full.log
LIST=${1:?usage: build_parallel_211.sh <list> [parallel]}
WIDTH=${2:-2}
mkdir -p "$DEST" "$CORPUS"

run_one() {
  local leg="$1"
  [ -f "$DEST/.done_$leg" ] && { echo "have $leg" >> "$LOG"; return 0; }
  local local_corpus=1
  if [ ! -d "$CORPUS/$leg" ]; then
    local_corpus=0
    [ -f "$TARS/$leg.tar" ] || { echo "NO TAR $leg" >> "$LOG"; return 1; }
    echo "---- restore $leg $(date +%T)" >> "$LOG"
    tar -xf "$TARS/$leg.tar" -C "$CORPUS" 2>/dev/null || { echo "extract failed $leg" >> "$LOG"; return 1; }
  fi
  echo "---- build $leg (local=$local_corpus) $(date +%T)" >> "$LOG"
  # THE LINEAR ALGEBRA LIBRARY SPWNS A THREAD PER CORE PER PROCESS unless told otherwise, so three legs on sixteen cores ask for forty-eight threads and the
  # load average reads forty-four while each build crawls. Four threads each leaves room for the replay pools and for the rest of the machine.
  if OMP_NUM_THREADS=${BLAS_THREADS:-4} OPENBLAS_NUM_THREADS=${BLAS_THREADS:-4} MKL_NUM_THREADS=${BLAS_THREADS:-4} \
     TRAIN=$TRAIN DATASETS_DIR=$DEST STATE=$TRAIN/bins_state_full.json \
     # THE REPLAY POOL IS WHAT SATURATES THE CORES, not the build. One leg's dump runs JOBS unit-shells at once, so N legs x JOBS is the real thread count.
     # Measured 2026-10-31: two legs with six each already put the load at thirteen on sixteen threads, and a third leg without lowering JOBS would only queue
     # behind the same cores. Twelve threads split three ways keeps the total while letting three legs advance instead of two.
     Q_SELF=100000000 Q_HARQ=100000000 JOBS=${JOBS_PER_LEG:-6} \
     "$TOOLS/queue_build_datasets.sh" "$leg" >> "$LOG" 2>&1; then
    touch "$DEST/.done_$leg"
    echo "     done $leg: $(ls "$DEST/${leg}"_*.npz 2>/dev/null | wc -l) npz $(date +%T)" >> "$LOG"
  else
    echo "     FAILED $leg $(date +%T)" >> "$LOG"
  fi
  [ "$local_corpus" = 0 ] && rm -rf "$CORPUS/$leg"
  return 0
}
export -f run_one
export TRAIN TOOLS DEST CORPUS TARS LOG

# xargs keeps WIDTH legs in flight; each writes to its own dump directory and its own arrays.
tr '\n' '\0' < "$LIST" | xargs -0 -n 1 -P "$WIDTH" bash -c 'run_one "$1"' _
echo "===== parallel list done $(date +%T)" >> "$LOG"
