#!/usr/bin/env bash
# OFFLINE BUILD QUEUE: dumps first, then bin-driven selection and build.
#
# WHY TWO PHASES. A dump decodes the whole leg once and writes an index; the selection then reads only the index and the PDU sidecars, and
# can be re-run in minutes. On 2026-10-28 the first phase ran for every leg and the second produced nothing -- the selector looked for
# <root>/<leg> where the queue writes <root>/tb_<leg>, and took its leg list from a process substitution that read as empty -- so a night of
# dumps looked like a night of nothing. With both bugs fixed the same dumps produced 101364 samples. Keeping the phases separate is what made
# the repair cheap.
#
# QUOTAS. Self 2500 and harq 250 per bin: ten to one, which keeps a base library large enough to draw a small experiment from and leaves the
# rare recovered samples scarce. A bin smaller than its quota gives what it has; the quota is a ceiling, not a promise.
#
# Usage: queue_build_datasets.sh <leg> [<leg> ...]
#   TRAIN (default ~/work/ai_phy_train) is the tree holding corpus/, tb_<leg>/ and datasets_bin/.
#   Q_SELF (2500), Q_HARQ (250), MIN_FREE_KB (15 GiB) and LEG_TB_PREFIX are the knobs.
set -u
TRAIN=${TRAIN:-$HOME/work/ai_phy_train}
PY=$TRAIN/.venv/bin/python
TOOLS=$(cd "$(dirname "$0")" && pwd)
STATE=$TRAIN/bins_state.json
Q_SELF=${Q_SELF:-2500}
Q_HARQ=${Q_HARQ:-250}
MIN_FREE_KB=${MIN_FREE_KB:-$((15*1024*1024))}
LEGS=${*:-}

[ -n "$LEGS" ] || { echo "usage: $0 <leg> [<leg> ...]" >&2; exit 2; }
# THE OUTPUT DIRECTORY AND THE QUOTA LEDGER ARE SEPARATE KNOBS, so that one can build the same legs under different sampling policies without the second
# build inheriting the first one's accounting. The default keeps the historical layout; DATASETS_DIR=... STATE=... gives an independent, unlimited build.
DATASETS_DIR=${DATASETS_DIR:-$TRAIN/datasets_bin}
STATE=${STATE:-$TRAIN/bins_state.json}
mkdir -p "$DATASETS_DIR"

free_kb() { df -k "$TRAIN" | tail -1 | awk '{print $4}'; }

echo "===== phase 1 (dumps) $(date +%T) free=$(df -h "$TRAIN" | tail -1 | awk '{print $4}')"
for LEG in $LEGS; do
  L=$TRAIN/corpus/$LEG; O=$TRAIN/tb_$LEG
  [ -d "$L" ] || { echo "absent $LEG"; continue; }
  if [ -s "$O/index.csv" ]; then echo "have $LEG ($(($(wc -l < "$O/index.csv") - 1)))"; continue; fi
  echo "---- dump $LEG $(date +%T)"
  mkdir -p "$O"
  "$TOOLS/dump_tb.sh" --out "$O" --jobs "${JOBS:-14}" "$L" 2>&1 | tail -1
  "$PY" "$TOOLS/harq_backfill.py" --leg "$L" --index "$O/index.csv" --live-tb "$L/tb_live" --out "$O" 2>&1 | tail -2
done

echo "===== phase 2 (select + build) $(date +%T) free=$(df -h "$TRAIN" | tail -1 | awk '{print $4}')"
for LEG in $LEGS; do
  L=$TRAIN/corpus/$LEG; O=$TRAIN/tb_$LEG
  [ -s "$O/index.csv" ] || continue
  if [ "$(free_kb)" -lt "$MIN_FREE_KB" ]; then
    echo "STOP: free space below the guard, keeping the dumps"; break
  fi
  # The leg name is passed LITERALLY: this tool also accepts a file, and a caller that passed a process substitution got an empty list.
  "$PY" "$TOOLS/select_by_bin.py" --legs "$LEG" --corpus "$TRAIN/corpus" --tb-root "$TRAIN" \
      --state "$STATE" --quota-self "$Q_SELF" --quota-harq "$Q_HARQ" 2>&1 | grep -E "selected|state|REFUSING|no dump" | tail -3
  for pop in self harq; do
    F=$O/index_sel_$pop.csv
    [ -s "$F" ] || continue
    for qm in QPSK 16QAM 64QAM 256QAM; do
      [ "$(free_kb)" -lt "$MIN_FREE_KB" ] && { echo "STOP (guard)"; break 3; }
      "$PY" "$TOOLS/build_depth3_dataset.py" --units "$F" --tb "$O/tb" --corpus "$L" \
          --out "$DATASETS_DIR" --name "${LEG}_${qm}_${pop}" --source real --modulation "$qm" \
          --min-prb 1 --max-units 0 2>&1 | tail -1
    done
  done
  echo "---- built $LEG $(date +%T) free=$(df -h "$TRAIN" | tail -1 | awk '{print $4}')"
done

echo "===== audit"
"$PY" "$TOOLS/check_depth3_dataset.py" --dir "$DATASETS_DIR" 2>&1 | tail -12
echo "===== done $(date +%T)"
