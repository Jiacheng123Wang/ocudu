#!/bin/bash
# Probe every new snapshot of a run on 500 ruler units, so the run measures itself while it trains.
#
# WHY. The validation cannot rank epochs -- the c32 run's validation preferred 46 while the ruler preferred
# 60 -- and a human watching a log has no better information. One probe costs about five minutes and covers
# 500 units, which separates a one-percent epoch from a thirteen-percent one comfortably, though not two
# adjacent epochs. That is the resolution this question needs.
#
# EVERY SNAPSHOT IS PROBED ONCE: the .done marker beside each result makes re-runs cheap and idempotent.
set -u
HOST=${HOST:-jwang@192.168.31.107}
RUN_DIR=${RUN_DIR:-/home/jwang/work/ai_phy_train/runs/w2c32_b18_e61}
UNITS=${UNITS:-500}
OUT=$HOME/ai_depth3/probe/results_$(basename "$RUN_DIR").txt
mkdir -p "$HOME/ai_depth3/probe"
touch "$OUT"
while true; do
  for path in $(ssh -o BatchMode=yes "$HOST" "ls $RUN_DIR/snapshots/*.keras 2>/dev/null" 2>/dev/null); do
    tag=$(basename "$path" .keras)
    [ -f "$HOME/ai_depth3/probe/$tag/.done" ] && continue
    echo "=== probing $tag $(date +%T)" >> "$OUT"
    if "$HOME/ai_depth3/bin/probe_snapshot.sh" "$HOST:$path" "$UNITS" 2>&1 | tee -a "$OUT" | grep -q "delta"; then
      mkdir -p "$HOME/ai_depth3/probe/$tag" && touch "$HOME/ai_depth3/probe/$tag/.done"
    fi
  done
  sleep 300
done
