#!/usr/bin/env bash
# Build every leg's mother set, LEGS AT A TIME, one process per leg.
#
# WHY LEGS IN PARALLEL AND NOT JOBS INSIDE A LEG. `build_mother_set.sh` can run its (modulation, source) pairs
# concurrently via JOBS, but most of our legs carry ONE modulation, so JOBS buys nothing there and the machine sits
# idle. The unit that parallelises well is the leg: each is an independent pair of directories and writes its own
# npz files, so N legs at once use N cores and N times the memory (measured 2-3 GB per build with CHUNK=8000).
# On the 14-core Mac the default is 10 concurrent legs: ~30 GB, leaving headroom for the training that may also run.
#
# WHAT COUNTS AS DONE. A leg gets a marker `<out>/.done_<leg>` only after `build_mother_set.sh` exits zero, because
# that script fails the leg if any pair failed or the audit reported findings. The npz files alone are not proof:
# a leg interrupted during a merge leaves `.npz.tmp` and parts behind.
#
# THE DISK IS THE THING THAT RUNS OUT. Each leg lands 2-3 GB, and the parts plus the merged file peak at about twice
# that per leg in flight, so the driver refuses to start a new leg below --min-free GB (default 60).
#
# Usage: build_mother_all.sh <dump-root> <corpus-root> <out> [--legs N] [--jobs J] [--min-free GB]
set -u
DUMP_ROOT=${1:?usage: build_mother_all.sh <dump-root> <corpus-root> <out> [--legs N]}
CORPUS_ROOT=${2:?}
OUT=${3:?}
shift 3
LEGS=10
JOBS=1
MIN_FREE=20
while [ $# -gt 0 ]; do
  case "$1" in
    --legs) LEGS=$2; shift 2 ;;
    --jobs) JOBS=$2; shift 2 ;;
    --min-free) MIN_FREE=$2; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
HERE=$(cd "$(dirname "$0")" && pwd)
# THE BUILDER IS NOT IN THIS DIRECTORY. `build_mother_set.sh` ships with the trainer (in the repository's ai_train
# directory), while this driver lives in the workflow tools; the first version pointed at $HERE and every leg died
# with "No such file or directory" in the same second. LIB names where the trainer's scripts actually are.
LIB=${LIB:-$HOME/dev/ocudu/lib/phy/upper/ai_signal_processors/depth3/ai_train}
TRAIN=${TRAIN:-$HOME/dev/ocudu}
PY=${PY:-$HOME/ai_ce_work/venv/bin/python}
mkdir -p "$OUT/logs"
LOG=$OUT/build_all.log
say() { echo "[$(date '+%F %T')] $*" >> "$LOG"; }
say "=== pool start: legs=$LEGS jobs=$JOBS dump=$DUMP_ROOT corpus=$CORPUS_ROOT out=$OUT"

export TRAIN PY DUMP_ROOT CORPUS_ROOT OUT JOBS
export CHUNK=${CHUNK:-8000}

running=0
for dump in "$DUMP_ROOT"/tb_*/; do
  leg=$(basename "$dump"); leg=${leg#tb_}
  marker=$OUT/.done_$leg
  if [ -e "$marker" ]; then say "skip $leg (done)"; continue; fi
  if [ ! -f "$dump/index.csv" ]; then say "skip $leg (not dumped)"; continue; fi
  if [ ! -d "$CORPUS_ROOT/$leg" ]; then say "skip $leg (no captures)"; continue; fi
  while [ "$running" -ge "$LEGS" ]; do running=$(jobs -rp | wc -l | tr -d ' '); sleep 5; done
  free_gb=$(df -g "$OUT" | awk 'NR==2 {print $4}')
  while [ "${free_gb:-999}" -lt "$MIN_FREE" ]; do
    say "--- waiting: free ${free_gb} GB < ${MIN_FREE} GB"
    sleep 120
    free_gb=$(df -g "$OUT" | awk 'NR==2 {print $4}')
  done
  (
    if bash "$LIB/build_mother_set.sh" "$leg" > "$OUT/logs/$leg.log" 2>&1; then
      touch "$marker"
      n=$(ls "$OUT/${leg}_"*.npz 2>/dev/null | wc -l | tr -d ' ')
      size=$(du -sk "$OUT/${leg}_"*.npz 2>/dev/null | awk '{s+=$1} END {printf "%.1f", s/1048576}')
      echo "[$(date '+%F %T')] [$leg] OK npz=$n ${size} GB" >> "$LOG"
    else
      echo "[$(date '+%F %T')] [$leg] FAILED (see logs/$leg.log)" >> "$LOG"
    fi
  ) &
  running=$(jobs -rp | wc -l | tr -d ' ')
  say "--- spawned $leg ($running in flight)"
done
wait
say "=== pool done: $(ls "$OUT"/.done_* 2>/dev/null | wc -l | tr -d ' ') leg(s) marked done"
