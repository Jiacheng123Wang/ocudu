#!/usr/bin/env bash
# Dump the decoded transport block of every corpus unit the classical chain can decode.
#
# WHY. A real corpus sample needs the bits the phone actually transmitted, and the only place they exist is a successful
# decode. The chain therefore writes the TB whenever its own CRC passes (OCUDU_HELENA_DUMP_DIR), and that file becomes
# the label. This is the step that turns captures into trainable units, and it is why a capture the reference chain
# cannot decode has no label of its own (design section 19.626).
#
# IT RUNS ANYWHERE THE CHAIN BUILDS. The default binary is the CPU-only target -- ul_chain_replay_cpu -- which is what
# the Linux machine has (section D7.7). The Apple-only metal build works too, but nothing here needs Metal: the dump is
# the CLASSICAL receiver's verdict, so the pipeline and the backend are pinned to cpu/classic rather than left to a
# default that could quietly measure the AI arm instead.
#
# Output (all under --out):  tb/<unit>.bits, index.csv  (unit,tb_bits,crc,sinr_db,iterations,slot)
# Usage: dump_tb.sh [--corpus DIR] [--out DIR] [--limit N] [--binary PATH] [--jobs N] [unit-dir ...]
#
# --jobs N runs N units at a time. Each unit is one replay process, so this is where the machine's cores are spent: the
#   dump was single-threaded and left fifteen of them idle on the sixteen-core host (measured 2026-10-27). Results do not
#   depend on the order, because every unit writes its own row and its own label file, and the index is assembled in the
#   sorted order afterwards.
set -uo pipefail

SELF_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$SELF_DIR/../../../../../.." && pwd)
BINARY="$ROOT/build/lib/phy/upper/channel_processors/ul_chain_replay_cpu"
[ -x "$BINARY" ] || BINARY="$ROOT/build/lib/phy/upper/channel_processors/metal/ul_chain_replay"
CORPUS=$ROOT/doc_chinese/llr_ai_detection/work_tmp/corpus
OUT=/tmp/tb_corpus
LIMIT=0
JOBS=1
DIRS=()

while [ $# -gt 0 ]; do
  case "$1" in
    --corpus) CORPUS="$2"; shift 2 ;;
    --out)    OUT="$2"; shift 2 ;;
    --limit)  LIMIT="$2"; shift 2 ;;
    --binary) BINARY="$2"; shift 2 ;;
    --jobs)   JOBS="$2"; shift 2 ;;
    *) DIRS+=("$1"); shift ;;
  esac
done

[ -x "$BINARY" ] || { echo "no replay binary at $BINARY" >&2; exit 2; }
[ ${#DIRS[@]} -gt 0 ] || DIRS=("$CORPUS/col_1025_verify")

mkdir -p "$OUT/tb" "$OUT/stage" "$OUT/raw"
: > "$OUT/index.csv"
echo "unit,tb_bits,crc,sinr_db,iterations,slot,tbs_bytes" >> "$OUT/index.csv"

n_attempt=0
n_ok=0
one_unit() {
  # Everything a unit touches lives under its own name, so two workers never meet: the dump directory, the staged
  # capture and the row file are all per unit. OUT and BINARY are exported rather than passed, because `xargs` appends
  # its item AFTER the fixed arguments and would otherwise hand `one_unit` the wrong ones.
  local meta="$1" out="$OUT" binary="$BINARY"
  local unit="${meta%.txt}"
  local name
  name=$(basename "$unit")
  [ -f "$unit.bin" ] || return 0

  local work="$out/raw/$name"
  rm -rf "$work"; mkdir -p "$work"
  local line
  line=$(OCUDU_HELENA_DUMP_DIR="$work" OMP_NUM_THREADS=1 "$binary" "$unit" \
           --phy_pipeline cpu --pusch_receiver_backend classic --out "$out/stage/$name" 2>/dev/null \
         | grep -E '^\s*\[0\]' | head -1)

  local slot crc=no sinr="" iters="" tbbits=0 tbs=""
  slot=$(grep -m1 '^slot=' "$meta" | cut -d= -f2)
  local tb
  tb=$(ls "$work"/tb_*.bits 2>/dev/null | head -1)
  if [ -n "$tb" ]; then
    cp "$tb" "$out/tb/$name.bits"
    tbbits=$(( $(wc -c < "$tb") * 8 ))
  fi
  if [ -n "$line" ]; then
    case "$line" in *"crc=OK"*) crc=OK ;; esac
    sinr=$(printf '%s' "$line"  | sed -n 's/.*sinr=\([-0-9.]*\) dB.*/\1/p')
    iters=$(printf '%s' "$line" | sed -n 's/.*iterations=\([0-9]*\).*/\1/p')
    tbs=$(printf '%s' "$line" | sed -n 's/.*tbs=\([0-9]*\).*/\1/p')
  fi
  rm -rf "$work"
  printf '%s,%s,%s,%s,%s,%s,%s\n' "$name" "$tbbits" "$crc" "$sinr" "$iters" "$slot" "$tbs" > "$out/rows/$name.row"
}
export -f one_unit

mkdir -p "$OUT/rows"
: > "$OUT/units.txt"
for dir in "${DIRS[@]}"; do
  # A GLOB WOULD DIE ON ARG_MAX HERE: a leg holds tens of thousands of files, and the shell silently hands `ls` an
  # empty list once the expansion is too long (measured: "attempted 0 units" on a 27k-file leg). `find` does not.
  # APPLE DOUBLE FILES ARE NOT UNITS. A corpus packed on a Mac carries "._cap_....txt" resource-fork stubs beside every real capture, and they match
  # "*.txt", so every index came out with as many junk rows as real ones -- measured 2026-10-30: 99 821 of 199 643 on one leg. They carry no crc and are
  # never selected, so the datasets were clean, but the index was twice the size it should be.
  find "$dir" -maxdepth 1 -name '*.txt' ! -name '*_ce.txt' ! -name '._*' 2>/dev/null | LC_ALL=C sort >> "$OUT/units.txt"
done
if [ "$LIMIT" -gt 0 ]; then
  head -n "$LIMIT" "$OUT/units.txt" > "$OUT/units.limited"; mv "$OUT/units.limited" "$OUT/units.txt"
fi

# The pool. xargs keeps JOBS units in flight; each writes its own row, so nothing needs a lock.
echo "  $(wc -l < "$OUT/units.txt") unit(s), $JOBS at a time" >&2
export OUT BINARY
tr '\n' '\0' < "$OUT/units.txt" | xargs -0 -n 1 -P "$JOBS" bash -c 'one_unit "$1"' _ 2>/dev/null

# Index in the sorted order the unit list already has, so two runs produce the same file.
for meta in $(cat "$OUT/units.txt"); do
  name=$(basename "${meta%.txt}")
  [ -f "$OUT/rows/$name.row" ] && cat "$OUT/rows/$name.row" >> "$OUT/index.csv"
done

n_attempt=$(wc -l < "$OUT/units.txt" | tr -d ' ')
n_ok=$(grep -c ',OK,' "$OUT/index.csv" 2>/dev/null || true)
echo "attempted $n_attempt units, ${n_ok:-0} decoded CRC-OK"
echo "TB files in $OUT/tb ($(ls "$OUT/tb" | wc -l | tr -d ' '))"
