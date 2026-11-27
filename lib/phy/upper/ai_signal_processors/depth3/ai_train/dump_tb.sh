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
# Output (all under --out):  tb/<unit>.bits, index.csv  (unit,tb_bits,crc,sinr_db,iterations,iterations_max,slot,tbs)
#
# THE SCRATCH IS NOT A PRODUCT. stage/ holds the replay's own per-unit output and raw/ its per-unit dump directory; nothing
# reads either after its unit finishes, so each unit's stage directory is removed as soon as its row and bits are written,
# and rows/, stage/ and raw/ are removed when the run ends. That keeps the peak footprint at the corpus plus the contract
# products instead of adding ~6 GB of captures a second time. --keep-scratch leaves everything in place for a post-mortem.
#
# THE DUMP HOLDS BOTH POPULATIONS. A first transmission the classical chain fails has no label of its own, but the same
# block's later retransmission decodes with soft combining and supplies it. That decode only exists in one process
# walking the whole HARQ chain, so the standard dump runs a second pass over the chains that failed (`--no-harq` skips
# it) and writes the recovered blocks into the same tb/ directory, then the two populations apart in index_self.csv and
# index_harq_only.csv. What neither decode labelled lands in index_ko.csv, which is the work list for another decoder: with
# the captures beside it, a GPU or NPU back end can try the receptions the classical chain could not decode. The live chain
# cannot work this way -- it must decode every first transmission to know whether to ask for a retransmission -- but replay
# can, because the chain is already on disk.
#
# Usage: dump_tb.sh [--corpus DIR] [--out DIR] [--limit N] [--binary PATH] [--jobs N] [--no-harq] [--keep-scratch]
#                    [unit-dir ...]
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
WITH_HARQ=1
KEEP_SCRATCH=0
DIRS=()

while [ $# -gt 0 ]; do
  case "$1" in
    --corpus) CORPUS="$2"; shift 2 ;;
    --out)    OUT="$2"; shift 2 ;;
    --limit)  LIMIT="$2"; shift 2 ;;
    --binary) BINARY="$2"; shift 2 ;;
    --jobs)   JOBS="$2"; shift 2 ;;
    --no-harq) WITH_HARQ=0; shift ;;
    --keep-scratch) KEEP_SCRATCH=1; shift ;;
    *) DIRS+=("$1"); shift ;;
  esac
done

[ -x "$BINARY" ] || { echo "no replay binary at $BINARY" >&2; exit 2; }
[ ${#DIRS[@]} -gt 0 ] || DIRS=("$CORPUS/col_1025_verify")

mkdir -p "$OUT/tb" "$OUT/stage" "$OUT/raw"
: > "$OUT/index.csv"
echo "unit,tb_bits,crc,sinr_db,iterations,iterations_max,slot,tbs_bytes" >> "$OUT/index.csv"

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

  local slot crc=no sinr="" iters="" itersmax="" tbbits=0 tbs=""
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
    # THE WORST CODEBLOCK'S ITERATION COUNT. `iterations` above is the mean over the transport block's codeblocks, so a
    # five-codeblock block whose hardest member needed seven reports a mean of four -- which is why the harvest cannot
    # use the mean as a difficulty label. This column keeps that number.
    itersmax=$(printf '%s' "$line" | sed -n 's/.*iterations_max=\([0-9]*\).*/\1/p')
    tbs=$(printf '%s' "$line" | sed -n 's/.*tbs=\([0-9]*\).*/\1/p')
  fi
  rm -rf "$work"
  # THE STAGED CAPTURE IS SPENT ONCE THE ROW AND THE BITS ARE WRITTEN. Removing it here keeps a serial run from holding
  # one leg's captures twice: the 5.8 GB that stage/ would otherwise accumulate is the largest single artifact the dump
  # produces and nothing reads it back.
  [ "$KEEP_SCRATCH" -eq 1 ] || rm -rf "$out/stage/$name"
  printf '%s,%s,%s,%s,%s,%s,%s,%s\n' "$name" "$tbbits" "$crc" "$sinr" "$iters" "$itersmax" "$slot" "$tbs" > "$out/rows/$name.row"
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
export OUT BINARY KEEP_SCRATCH
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

# THE RECOVERED HALF. Chains whose first transmission failed are replayed whole, so the soft buffer carries the earlier
# receptions and the block decodes; the bits land in the same tb/ and the combined decode's iteration counts replace the
# failed attempt's (which saturate at the cap and say nothing about the block). One directory per run: the chain pass and
# the labelling both need the capture directory they belong to.
if [ "$WITH_HARQ" -eq 1 ] && [ ${#DIRS[@]} -eq 1 ]; then
  LEG_DIR="${DIRS[0]}"
  CHAIN_CSV="$OUT/chain_iters.csv"
  PYTHON=${PY:-python3}
  # An empty array expansion is an unbound variable under `set -u` in the bash 3.2 this Mac ships, so the flag list is
  # expanded with the guarded form.
  LIVE_ARGS=()
  [ -d "$LEG_DIR/tb_live" ] && LIVE_ARGS=(--live-tb "$LEG_DIR/tb_live")
  echo "  recovered half: replaying HARQ chains under $LEG_DIR" >&2
  "$PYTHON" "$SELF_DIR/dump_harq_chains.py" --leg "$LEG_DIR" --index "$OUT/index.csv" \
      --out "$CHAIN_CSV" --jobs "$JOBS" --tb-out "$OUT/tb" \
      "${LIVE_ARGS[@]+"${LIVE_ARGS[@]}"}" || echo "  WARN: chain pass failed" >&2
  if [ -f "$CHAIN_CSV" ]; then
    "$PYTHON" "$SELF_DIR/harq_backfill.py" --leg "$LEG_DIR" --index "$OUT/index.csv" --out "$OUT" \
        --chain-iters "$CHAIN_CSV" "${LIVE_ARGS[@]+"${LIVE_ARGS[@]}"}" --max-gap-slots 200 \
      || echo "  WARN: labelling failed" >&2
    n_self=$(wc -l < "$OUT/index_self.csv" 2>/dev/null | tr -d ' ')
    n_harq=$(wc -l < "$OUT/index_harq_only.csv" 2>/dev/null | tr -d ' ')
    n_ko=$(wc -l < "$OUT/index_ko.csv" 2>/dev/null | tr -d ' ')
    echo "  populations: self=$(( ${n_self:-1} - 1 )) harq=$(( ${n_harq:-1} - 1 )) ko=$(( ${n_ko:-1} - 1 )) (ko = no label; work list for another decoder)"
    echo "  tb now holds $(ls "$OUT/tb" | wc -l | tr -d ' ') blocks"
  fi
fi

# THE DRAFT DIRECTORIES GO LAST, after the index is assembled and the populations are split: rows/ is what index.csv was
# built from, so it is only redundant once that file exists. A run that died earlier keeps its scratch, which is what a
# post-mortem and a resumed index assembly need.
if [ "$KEEP_SCRATCH" -eq 0 ]; then
  rm -rf "$OUT/stage" "$OUT/rows" "$OUT/raw"
  echo "  scratch removed (stage/ rows/ raw/; pass --keep-scratch to keep them)"
fi
