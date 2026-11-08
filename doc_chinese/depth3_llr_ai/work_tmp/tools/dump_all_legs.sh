#!/usr/bin/env bash
# Dump every capture leg under one tree, one output directory per leg, resuming where it stopped.
#
# WHY A LOOP AND NOT 74 COMMANDS. The dump is per-leg (`dump_tb.sh` refuses a root directory, because segment names
# repeat across legs and the labels would mix), the work is hours long, and the machine is also the one that flies the
# phone. A loop that skips finished legs, logs one line of reconciliation per leg and can be paused between legs is
# what makes that survivable: a crash or a reboot costs one leg, not the run.
#
# PAUSING. Create `<out>/PAUSE` and the loop stops before starting the next leg; remove it and the loop continues. To
# stop a leg already in flight, `kill -STOP` the process tree (see tools/pause_dump.sh). Nothing is lost either way:
# each leg writes its own directory and is skipped only when its reconciliation passes.
#
# DISK. `stage/` and `rows/` are scratch (the manual calls them deletable drafts) and are removed after every leg;
# keeping them would cost ~5.5 GB per leg, which does not fit for 74 legs. What is kept is `tb/`, `index.csv`,
# `units.txt` -- about 180 MB per leg at 50 000 units.
#
# Usage: dump_all_legs.sh <corpus-root> <out-root> [--jobs N] [--binary PATH]
set -u
CORPUS=${1:?usage: dump_all_legs.sh <corpus-root> <out-root> [--jobs N] [--binary PATH]}
OUT=${2:?usage: dump_all_legs.sh <corpus-root> <out-root> [--jobs N] [--binary PATH]}
shift 2
JOBS=3
BINARY=${HOME}/dev/ocudu/build/lib/phy/upper/channel_processors/ul_chain_replay_cpu
while [ $# -gt 0 ]; do
  case "$1" in
    --jobs)   JOBS=$2; shift 2 ;;
    --binary) BINARY=$2; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
LIB=$(cd "$(dirname "$0")" && pwd)/../../../../lib/phy/upper/ai_signal_processors/depth3/ai_train
LIB=$(cd "$LIB" && pwd)
mkdir -p "$OUT"
LOG=$OUT/dump_all.log
say() { echo "[$(date '+%F %T')] $*" >> "$LOG"; }

say "=== start: corpus=$CORPUS out=$OUT jobs=$JOBS binary=$BINARY"
for leg_path in "$CORPUS"/*/; do
  leg=$(basename "$leg_path")
  out=$OUT/tb_$leg
  # THE PAUSE FILE IS CHECKED BETWEEN LEGS, so pausing never leaves a half-written index behind.
  while [ -e "$OUT/PAUSE" ]; do sleep 20; done
  if [ -s "$out/index.csv" ] && [ -s "$out/units.txt" ]; then
    units=$(wc -l < "$out/units.txt" | tr -d ' ')
    rows=$(( $(wc -l < "$out/index.csv") - 1 ))
    if [ "$rows" -eq "$units" ]; then
      say "skip $leg (complete: units=$units rows=$rows)"
      continue
    fi
    say "redo $leg (units=$units rows=$rows -- reconciliation failed)"
  fi
  say "=== dump $leg"
  nice -n 20 bash "$LIB/dump_tb.sh" --corpus "$CORPUS" --out "$out" --jobs "$JOBS" \
      --binary "$BINARY" "$leg_path" >> "$LOG" 2>&1
  status=$?
  rm -rf "$out/stage" "$out/rows"
  units=$( [ -s "$out/units.txt" ] && wc -l < "$out/units.txt" | tr -d ' ' || echo 0 )
  rows=$(( $( [ -s "$out/index.csv" ] && wc -l < "$out/index.csv" || echo 1) - 1 ))
  ok=$( [ -s "$out/index.csv" ] && grep -c ',OK,' "$out/index.csv" || echo 0 )
  say "--- $leg: status=$status units=$units index_rows=$rows crc_ok=$ok"
done
say "=== all legs done"
