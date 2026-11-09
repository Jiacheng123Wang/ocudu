#!/usr/bin/env bash
# Dump ONE capture leg into <out>/tb_<leg>, then reconcile, then drop the scratch directories.
#
# Split out of the driver so several legs can run at once: the driver starts one of these per leg and the operating
# system does the scheduling. Every line it prints goes to the shared log, prefixed with the leg, because four legs
# writing at once into one file is unreadable otherwise.
#
# Usage: dump_one_leg.sh <corpus-root> <out-root> <leg> <jobs> <binary>
set -u
CORPUS=${1:?}; OUT=${2:?}; LEG=${3:?}; JOBS=${4:-3}; BINARY=${5:?}
LIB=$(cd "$(dirname "$0")" && pwd)/../../../../lib/phy/upper/ai_signal_processors/depth3/ai_train
LIB=$(cd "$LIB" && pwd)
LOG=$OUT/dump_all.log
say() { echo "[$(date '+%F %T')] [$LEG] $*" >> "$LOG"; }

out=$OUT/tb_$LEG
say "=== dump start (jobs=$JOBS)"
nice -n 20 bash "$LIB/dump_tb.sh" --corpus "$CORPUS" --out "$out" --jobs "$JOBS" \
    --binary "$BINARY" "$CORPUS/$LEG" >> "$LOG" 2>&1
status=$?
# THE SCRATCH IS DELETED IN THE BACKGROUND, AND THAT IS WHAT KEEPS THE POOL FULL. `stage/` reaches 3-7 GB per leg
# (a directory and a few files per unit, ~180 000 inodes), so an inline `rm -rf` stalls a worker for ten-plus minutes
# after its dump has finished: four legs then look "done" while the machine sits at low CPU, which is exactly how the
# first pool appeared to stop. The worker returns as soon as its reconciliation is written; the driver watches the
# free space and throttles if the deletions ever fall behind.
nice -n 10 rm -rf "$out/stage" "$out/rows" > /dev/null 2>&1 &
say "cleanup detached (stage + rows)"
units=$( [ -s "$out/units.txt" ] && wc -l < "$out/units.txt" | tr -d ' ' || echo 0 )
rows=$(( $( [ -s "$out/index.csv" ] && wc -l < "$out/index.csv" || echo 1) - 1 ))
ok=$( [ -s "$out/index.csv" ] && grep -c ',OK,' "$out/index.csv" || echo 0 )
say "--- done: status=$status units=$units index_rows=$rows crc_ok=$ok"
