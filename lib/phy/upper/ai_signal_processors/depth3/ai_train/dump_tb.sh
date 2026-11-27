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
# THE DECODER STRENGTH IS PART OF THE RESULT. The LDPC cap decides verdicts, not only latency: a reception that needs a
# seventh iteration is crc=KO at six and crc=OK at twenty. A dump therefore runs TWENTY iterations by default, because the
# extra iterations recover samples the air chain cannot decode and a dump is harvested for labels. Twenty is not the air
# configuration (every air leg runs six), so these products are a label harvest and not the reference verdict: pass
# OCUDU_REPLAY_DEC_ITERATIONS=6 for a reference dump, and read dump_meta.txt to see which one a directory holds. Both
# passes inherit the variable.
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
#                    (OCUDU_REPLAY_DEC_ITERATIONS=N selects the LDPC cap; default 20, 6 is the air configuration)
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

# TWENTY BY DEFAULT, SIX FOR THE AIR CONFIGURATION. The knob is exported so that both the per-unit pass and the chain pass
# run the same decoder strength; a caller that sets it keeps its value.
export OCUDU_REPLAY_DEC_ITERATIONS=${OCUDU_REPLAY_DEC_ITERATIONS:-20}

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
# PROVENANCE FIRST, BECAUSE THE CAP CHANGES THE VERDICTS. A reader of these products has to be able to tell a dump made at
# the air configuration from one made by a stronger decoder, and nothing else in the directory says which it is.
{
  echo "started=$(date '+%Y-%m-%d %H:%M:%S')"
  echo "ldpc_iterations=${OCUDU_REPLAY_DEC_ITERATIONS:-20}"
  echo "corpus=${CORPUS}"
  echo "out=${OUT}"
  echo "binary=${BINARY}"
  # GNU stat (the Linux replay host) and BSD stat (the Mac) disagree about the flag: `-c '%y'` is GNU's,
  # `-f '%Sm'` is BSD's, and GNU reads `-f` as "filesystem", succeeds, and fills the field with garbage --
  # which defeats the `|| unknown` fallback and leaves the dump without a usable provenance stamp. Ask
  # GNU first and cut its fractional seconds, so both branches print the same second-resolution form.
  if binary_mtime=$(stat -c '%y' "$BINARY" 2>/dev/null); then
    binary_mtime=$(printf '%s' "$binary_mtime" | cut -c1-19)
  else
    binary_mtime=$(stat -f '%Sm' -t '%Y-%m-%d %H:%M:%S' "$BINARY" 2>/dev/null || echo unknown)
  fi
  echo "binary_mtime=${binary_mtime}"
  echo "jobs=${JOBS}"
  echo "with_harq=${WITH_HARQ}"
  echo "legs=${DIRS[*]:-}"
} > "$OUT/dump_meta.txt"
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

# RETRY THE HOLES. Under load a replay occasionally dies before printing its verdict, which leaves a row with no SINR; the
# same unit decodes when it is run again (measured on the w2 legs 2026-11-27: four of ten thousand units were lost that way
# while a GPU training job shared the machine, and each decoded on its own on the retry). Only first transmissions are
# retried, because a retransmission has no soft buffer to reserve and fails the same way every time.
: > "$OUT/retry.txt"
while read -r unit_path; do
  name=${unit_path##*/}
  name=${name%.txt}
  row="$OUT/rows/$name.row"
  [ -f "$row" ] || { echo "$unit_path" >> "$OUT/retry.txt"; continue; }
  [ -n "$(cut -d, -f4 "$row")" ] && continue
  [ "$(grep -m1 '^new_data=' "$unit_path" | cut -d= -f2)" = "1" ] && echo "$unit_path" >> "$OUT/retry.txt"
done < "$OUT/units.txt"
n_retry=$(wc -l < "$OUT/retry.txt" | tr -d ' ')
if [ "$n_retry" -gt 0 ]; then
  echo "  retrying $n_retry unit(s) that produced no verdict" >&2
  tr '\n' '\0' < "$OUT/retry.txt" | xargs -0 -n 1 -P "$JOBS" bash -c 'one_unit "$1"' _ 2>/dev/null
fi
rm -f "$OUT/retry.txt"

# Index in the sorted order the unit list already has, so two runs produce the same file.
# A PER-ROW cat IS A PER-ROW FORK, and a fork costs about 66 ms on a loaded host: measured on the Ubuntu replay host
# (2026-11-27), a 24 000-unit leg took 33 minutes to replay its units and then 25 more to assemble this index one `cat`
# at a time. The existence test stays in the shell because a unit whose replay died has no row; the concatenation is one
# batched cat, which xargs splits under ARG_MAX on its own.
: > "$OUT/rows.order"
while read -r unit_path; do
  name=${unit_path##*/}
  name=${name%.txt}
  [ -f "$OUT/rows/$name.row" ] && echo "$OUT/rows/$name.row" >> "$OUT/rows.order"
done < "$OUT/units.txt"
xargs cat < "$OUT/rows.order" >> "$OUT/index.csv"
rm -f "$OUT/rows.order"

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

if [ -f "$OUT/dump_meta.txt" ]; then
  {
    echo "finished=$(date '+%Y-%m-%d %H:%M:%S')"
    echo "units=$(wc -l < "$OUT/units.txt" | tr -d ' ')"
    echo "crc_ok=$(grep -c ',OK,' "$OUT/index.csv" 2>/dev/null || echo 0)"
    echo "tb_blocks=$(ls "$OUT/tb" | wc -l | tr -d ' ')"
    [ -f "$OUT/index_self.csv" ] && echo "self=$(($(wc -l < "$OUT/index_self.csv") - 1))"
    [ -f "$OUT/index_harq_only.csv" ] && echo "harq=$(($(wc -l < "$OUT/index_harq_only.csv") - 1))"
    [ -f "$OUT/index_ko.csv" ] && echo "ko=$(($(wc -l < "$OUT/index_ko.csv") - 1))"
  } >> "$OUT/dump_meta.txt"
fi

# THE DRAFT DIRECTORIES GO LAST, after the index is assembled and the populations are split: rows/ is what index.csv was
# built from, so it is only redundant once that file exists. A run that died earlier keeps its scratch, which is what a
# post-mortem and a resumed index assembly need.
if [ "$KEEP_SCRATCH" -eq 0 ]; then
  rm -rf "$OUT/stage" "$OUT/rows" "$OUT/raw"
  echo "  scratch removed (stage/ rows/ raw/; pass --keep-scratch to keep them)"
fi
