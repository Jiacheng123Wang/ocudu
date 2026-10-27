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
# Usage: dump_tb.sh [--corpus DIR] [--out DIR] [--limit N] [--binary PATH] [unit-dir ...]
set -uo pipefail

SELF_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$SELF_DIR/../../../../../.." && pwd)
BINARY="$ROOT/build/lib/phy/upper/channel_processors/ul_chain_replay_cpu"
[ -x "$BINARY" ] || BINARY="$ROOT/build/lib/phy/upper/channel_processors/metal/ul_chain_replay"
CORPUS=$ROOT/doc_chinese/llr_ai_detection/work_tmp/corpus
OUT=/tmp/tb_corpus
LIMIT=0
DIRS=()

while [ $# -gt 0 ]; do
  case "$1" in
    --corpus) CORPUS="$2"; shift 2 ;;
    --out)    OUT="$2"; shift 2 ;;
    --limit)  LIMIT="$2"; shift 2 ;;
    --binary) BINARY="$2"; shift 2 ;;
    *) DIRS+=("$1"); shift ;;
  esac
done

[ -x "$BINARY" ] || { echo "no replay binary at $BINARY" >&2; exit 2; }
[ ${#DIRS[@]} -gt 0 ] || DIRS=("$CORPUS/col_1025_verify")

mkdir -p "$OUT/tb" "$OUT/stage" "$OUT/raw"
: > "$OUT/index.csv"
echo "unit,tb_bits,crc,sinr_db,iterations,slot" >> "$OUT/index.csv"

n_attempt=0
n_ok=0
for dir in "${DIRS[@]}"; do
  while IFS= read -r meta; do
    unit="${meta%.txt}"
    name=$(basename "$unit")
    [ -f "$unit.bin" ] || continue          # a capture the dump budget cut short is a skip, not a failure
    if [ "$LIMIT" -gt 0 ] && [ "$n_attempt" -ge "$LIMIT" ]; then break 2; fi
    n_attempt=$((n_attempt + 1))

    # A fresh dump dir per unit: the chain's own dump index restarts at zero in each process.
    rm -rf "$OUT/raw/cur"; mkdir -p "$OUT/raw/cur"
    line=$(OCUDU_HELENA_DUMP_DIR="$OUT/raw/cur" "$BINARY" "$unit" \
             --phy_pipeline cpu --pusch_receiver_backend classic --out "$OUT/stage/$name" 2>/dev/null \
           | grep -E '^\s*\[0\]' | head -1)

    slot=$(grep -m1 '^slot=' "$meta" | cut -d= -f2)
    crc=no; sinr=""; iters=""; tbbits=0
    tb=$(ls "$OUT/raw/cur"/tb_*.bits 2>/dev/null | head -1)
    if [ -n "$tb" ]; then
      cp "$tb" "$OUT/tb/$name.bits"
      tbbits=$(( $(wc -c < "$tb") * 8 ))
    fi
    if [ -n "$line" ]; then
      case "$line" in *"crc=OK"*) crc=OK; n_ok=$((n_ok + 1)) ;; esac
      sinr=$(printf '%s' "$line"  | sed -n 's/.*sinr=\([-0-9.]*\) dB.*/\1/p')
      iters=$(printf '%s' "$line" | sed -n 's/.*iterations=\([0-9]*\).*/\1/p')
    fi
    echo "$name,$tbbits,$crc,$sinr,$iters,$slot" >> "$OUT/index.csv"
    [ $((n_attempt % 100)) -eq 0 ] && echo "  $n_attempt attempted, $n_ok decoded" >&2
  # A GLOB WOULD DIE ON ARG_MAX HERE: a leg holds tens of thousands of files, and the shell silently hands `ls` an
  # empty list once the expansion is too long (measured: "attempted 0 units" on a 27k-file leg). `find` does not.
  done < <(find "$dir" -maxdepth 1 -name '*.txt' ! -name '*_ce.txt' 2>/dev/null | LC_ALL=C sort)
done
echo "attempted $n_attempt units, $n_ok decoded CRC-OK"
echo "TB files in $OUT/tb ($(ls "$OUT/tb" | wc -l | tr -d ' '))"
