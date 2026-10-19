#!/usr/bin/env bash
# Replay every leg's OWN captures through both arms, so each leg yields a channel measure and a decode
# rate that do not depend on what ran live.
#
# WHY. A live pair compares two sessions, and in the 2026-10-19 batch the classical legs in two of three
# pairs carried far more QPSK than their neighbours, which left open whether the throughput gap was the
# arm or the channel. The captures each leg recorded solve that: the same 60 slots replayed through the
# classical chain give SINR and decode for the CHANNEL, and replayed through the AI bundle give them for
# the ARM, on identical received signals. Two legs can then be compared with the channel divided out.
#
# What it prints per leg: the classical publication median (the channel), its decode rate, the arm's
# publication median and delta, and the arm's decode rate. The delta SINR is the arm's bias on that
# channel, which is what the calibration was fitted to remove.
#
# Usage:
#   leg_replay.sh <batch> <model> <label> [<label> ...]      e.g. leg_replay.sh legs/c32dmrs <bundle> d1 c5
set -u -o pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
W=$ROOT/doc_chinese/llr_ai_detection/work_tmp
PROBE=$ROOT/doc_chinese/llr_ai_detection/probe
PY=$HOME/ai_ce_work/venv/bin/python

BATCH=${1:?usage: leg_replay.sh <batch> <model> <label> ...}
MODEL=${2:?}
shift 2

echo "=== per-leg replay of the leg's own captures (identical signals for both arms)"
echo "  leg   n  classical pub p50   arm pub p50   dSINR med   classic decode   arm decode"
for leg in "$@"; do
  dir=$BATCH/pat/$leg
  [ -d "$dir" ] || { echo "  $leg  MISSING $dir" >&2; continue; }
  csv=$W/cmp/leg_${leg}.csv
  "$PY" "$PROBE/attach_ab.py" --captures "$dir" --model "$MODEL" --out "$csv" \
      > "$W/cmp/leg_${leg}.txt" 2>&1 || { echo "  $leg  replay failed, see leg_${leg}.txt" >&2; continue; }
  "$PY" - "$csv" "$leg" <<'PY'
import csv, sys, statistics
path, label = sys.argv[1], sys.argv[2]
rows = [r for r in csv.DictReader(open(path)) if r.get("crc_ai")]
# First transmissions only: a replayed retransmission carries the same signal as its original, so
# counting both would weight the same channel twice.
rows = [r for r in rows if r.get("new_data") == "1"] or rows
n = len(rows)
if not n:
    print(f"  {label}  no rows"); raise SystemExit
cl = [float(r["sinr_classic"]) for r in rows]
ar = [float(r["sinr_ai"]) for r in rows]
d = [a - c for a, c in zip(ar, cl)]
cd = 100.0 * sum(r["crc_classic"] == "OK" for r in rows) / n
ad = 100.0 * sum(r["crc_ai"] == "OK" for r in rows) / n
print(f"  {label:<5} {n:>3} {statistics.median(cl):>16.2f} {statistics.median(ar):>13.2f} "
      f"{statistics.median(d):>+11.2f} {cd:>14.1f}% {ad:>11.1f}%")
PY
done
