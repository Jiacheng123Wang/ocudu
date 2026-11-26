#!/bin/bash
# Harvest the hard samples of one leg: replay ONLY the units the previous dump called crc=KO, at a raised LDPC cap.
#
# THE WORK LIST IS THE POINT. Re-running every unit would spend most of its time re-decoding blocks that already have a
# label; a leg measured at 35% KO means two thirds of that work is thrown away. So the list is built from the dump index
# and nothing else runs, which is also what makes the per-machine budget in the plan affordable.
#
# THE OUTPUT IS A NEW INDEX, not a rewrite: units, verdicts and iteration counts of the raised-cap pass, plus the TB bits
# it recovered. The old dump stays untouched, so a bad run can never damage a corpus.
#
# Usage: harvest_leg.sh <leg> <tb tarball> <corpus tarball> <work dir> [--jobs 8] [--cap 20] [--limit 0]
set -u
LEG=$1; TB_TAR=$2; CORPUS_TAR=$3; WORK=$4; shift 4
JOBS=8; CAP=20; LIMIT=0
while [ $# -gt 0 ]; do
  case "$1" in
    --jobs) JOBS=$2; shift 2 ;;
    --cap) CAP=$2; shift 2 ;;
    --limit) LIMIT=$2; shift 2 ;;
    *) echo "unknown option $1"; exit 2 ;;
  esac
done
BIN=${BIN:-$HOME/work/ai_phy_train/bin/ul_chain_replay_cpu}
OUT="$WORK/$LEG"
rm -rf "$OUT"; mkdir -p "$OUT/tb" "$OUT/rows" "$OUT/stage"

# 1. THE DUMP INDEX, from the tb tarball (it also carries the bits of the units the first pass decoded).
tar xzf "$TB_TAR" -C "$OUT" --wildcards "*/index.csv" 2>/dev/null
INDEX=$(find "$OUT" -name index.csv | head -1)
[ -n "$INDEX" ] || { echo "ABORT: no index.csv in $TB_TAR"; exit 1; }

# 2. THE KO LIST. `crc=OK` rows are exactly the ones that already have a label, so they are skipped outright.
awk -F, 'NR > 1 && $3 != "OK" { print $1 }' "$INDEX" > "$OUT/ko_units.txt"
echo "  $(( $(wc -l < "$INDEX") - 1 )) units in the dump, $(wc -l < "$OUT/ko_units.txt") are KO"
if [ "$LIMIT" -gt 0 ]; then
  head -n "$LIMIT" "$OUT/ko_units.txt" > "$OUT/ko_limited.txt"; mv "$OUT/ko_limited.txt" "$OUT/ko_units.txt"
  echo "  limited to $(wc -l < "$OUT/ko_units.txt") units"
fi

# 3. THE CAPTURES OF THOSE UNITS ONLY.
python3 "$(dirname "$0")/extract_units.py" "$CORPUS_TAR" "$OUT/ko_units.txt" "$OUT/corpus" "$LEG" || exit 1

# 4. REPLAY AT THE RAISED CAP. One process per unit; OMP_NUM_THREADS=1 keeps a unit from taking the whole machine,
# because the parallelism here is across units, not inside one.
one_unit() {
  local unit=$1
  local name; name=$(basename "$unit")
  local f="$OUT/corpus/$LEG/$name"
  [ -f "$f.bin" ] || { echo "$name,SKIP,,-1,-1,0" >> "$OUT/rows/$name.row"; return 0; }
  local work="$OUT/stage/$name"
  mkdir -p "$work"
  local line
  line=$(OCUDU_REPLAY_DEC_ITERATIONS="$CAP" OCUDU_HELENA_DUMP_DIR="$work" OMP_NUM_THREADS=1 \
           "$BIN" "$f" --phy_pipeline cpu --pusch_receiver_backend classic --out "$work/stage" 2>/dev/null \
         | grep -E '^\s*\[0\]' | head -1)
  local crc=KO iters=-1 itersmax=-1 tbbits=0
  # THE POPULATION, recorded per unit so the harvest can be split into the same self/harq populations the mother set
  # uses. `new_data` is the flag the air chain set for a first transmission; a retransmission carries rv > 0 and
  # new_data = 0. Without these three columns the harvested units could only be pooled, which is exactly what the mother
  # set does not do.
  local harqid rv newdata
  harqid=$(grep -m1 '^harq_id=' "$f.txt" 2>/dev/null | cut -d= -f2); harqid=${harqid:--1}
  rv=$(grep -m1 '^rv=' "$f.txt" 2>/dev/null | cut -d= -f2); rv=${rv:--1}
  newdata=$(grep -m1 '^new_data=' "$f.txt" 2>/dev/null | cut -d= -f2); newdata=${newdata:--1}
  if [ -n "$line" ]; then
    case "$line" in *"crc=OK"*) crc=OK ;; esac
    iters=$(printf '%s' "$line" | sed -n 's/.*iterations=\([0-9]*\).*/\1/p'); iters=${iters:--1}
    itersmax=$(printf '%s' "$line" | sed -n 's/.*iterations_max=\([0-9]*\).*/\1/p'); itersmax=${itersmax:--1}
  fi
  local tb; tb=$(ls "$work"/tb_*.bits 2>/dev/null | head -1)
  if [ -n "$tb" ]; then cp "$tb" "$OUT/tb/$name.bits"; tbbits=$(( $(wc -c < "$tb") * 8 )); fi
  rm -rf "$work"
  echo "$name,$crc,$iters,$itersmax,$tbbits,$harqid,$rv,$newdata" > "$OUT/rows/$name.row"
}
export -f one_unit
export OUT LEG BIN CAP
tr '\n' '\0' < "$OUT/ko_units.txt" | xargs -0 -n 1 -P "$JOBS" bash -c 'one_unit "$1"' _ 2>/dev/null

# 5. THE INDEX, in the order the KO list has, so two runs of the same leg agree byte for byte.
echo "unit,crc,iterations,iterations_max,tb_bits,harq_id,rv,new_data" > "$OUT/index_hard.csv"
while read -r unit; do
  name=$(basename "$unit")
  [ -f "$OUT/rows/$name.row" ] && cat "$OUT/rows/$name.row" >> "$OUT/index_hard.csv"
done < "$OUT/ko_units.txt"
rm -rf "$OUT/stage" "$OUT/rows"

n_ko=$(($(wc -l < "$OUT/index_hard.csv") - 1))
n_ok=$(grep -c ',OK,' "$OUT/index_hard.csv" || true)
echo "  harvested: $n_ko KO units replayed, ${n_ok:-0} decoded at cap $CAP"
echo "  index: $OUT/index_hard.csv   bits: $(ls "$OUT/tb" | wc -l | tr -d ' ')"
