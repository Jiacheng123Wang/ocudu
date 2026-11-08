#!/usr/bin/env bash
# Dump every capture leg with a POOL OF LEGS: `--legs N` legs at a time, `--jobs J` replay processes per leg.
#
# WHY PARALLEL. One leg at `--jobs 3` runs at about 33 units/s, and the corpus is 74 legs of ~50 000 units, so the
# sequential loop needs more than a day. The unit is a single-threaded process, so the only way to use a 14-core box is
# to overlap legs as well as units. `model_network_design` recorded the measured limit for this workload: four legs at
# three workers each (twelve processes) already saturates fourteen cores, so the default is 4 x 3 and the driver refuses
# to start with legs*jobs above --max-concurrency (default: cores - 2).
#
# WHAT DOES NOT CHANGE. Each leg still writes its own directory, its own index and its own reconciliation line; a leg is
# skipped only when its index and unit list agree; the scratch directories are removed per leg. A crash costs the legs
# in flight, not the run.
#
# PAUSING. `<out>/PAUSE` stops the driver before it starts the next leg; legs already running finish. `pause_dump.sh
# stop|go` SIGSTOPs/SIGCONTs the whole tree for an immediate brake (used when the phone is in the air and the radio
# shares its USB bandwidth with this disk).
#
# Usage: dump_legs_parallel.sh <corpus-root> <out-root> [--legs N] [--jobs J] [--binary PATH] [--max-concurrency M]
set -u
CORPUS=${1:?usage: dump_legs_parallel.sh <corpus-root> <out-root> [--legs N] [--jobs J] [--binary PATH]}
OUT=${2:?usage: dump_legs_parallel.sh <corpus-root> <out-root> [--legs N] [--jobs J] [--binary PATH]}
shift 2
LEGS=4
JOBS=3
MAXCON=$(( $(sysctl -n hw.ncpu) - 2 ))
BINARY=${HOME}/dev/ocudu/build/lib/phy/upper/channel_processors/ul_chain_replay_cpu
while [ $# -gt 0 ]; do
  case "$1" in
    --legs) LEGS=$2; shift 2 ;;
    --jobs) JOBS=$2; shift 2 ;;
    --binary) BINARY=$2; shift 2 ;;
    --max-concurrency) MAXCON=$2; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
if [ $((LEGS * JOBS)) -gt "$MAXCON" ]; then
  echo "REFUSING: --legs $LEGS x --jobs $JOBS = $((LEGS * JOBS)) processes, above --max-concurrency $MAXCON" >&2
  exit 2
fi
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT"
LOG=$OUT/dump_all.log
say() { echo "[$(date '+%F %T')] $*" >> "$LOG"; }
say "=== pool start: legs=$LEGS jobs=$JOBS (concurrency $((LEGS * JOBS)) of $MAXCON) corpus=$CORPUS"

running=0
for leg_path in "$CORPUS"/*/; do
  leg=$(basename "$leg_path")
  out=$OUT/tb_$leg
  while [ -e "$OUT/PAUSE" ]; do sleep 20; done
  if [ -s "$out/index.csv" ] && [ -s "$out/units.txt" ]; then
    units=$(wc -l < "$out/units.txt" | tr -d ' ')
    rows=$(( $(wc -l < "$out/index.csv") - 1 ))
    if [ "$rows" -eq "$units" ]; then
      say "skip $leg (complete: units=$units rows=$rows)"
      continue
    fi
  fi
  while [ "$running" -ge "$LEGS" ]; do
    running=$(jobs -rp | wc -l | tr -d ' ')
    sleep 5
  done
  bash "$HERE/dump_one_leg.sh" "$CORPUS" "$OUT" "$leg" "$JOBS" "$BINARY" &
  running=$(jobs -rp | wc -l | tr -d ' ')
  say "--- spawned $leg ($running legs in flight)"
done
wait
say "=== pool done"
