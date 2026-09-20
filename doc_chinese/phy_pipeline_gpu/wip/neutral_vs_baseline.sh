#!/usr/bin/env bash
# The NEUTRALITY net: does the current build still publish the ARCHIVED baseline dumps, byte for byte?
#
# ---- What this is for ----
# Every change to the engine's host side has to answer one question before anything is measured on air:
# does it move a single published byte of a hop the change was not about? The baselines to compare
# against are the archived ones (doc_chinese/work_tmp/README.md):
#
#   * doc_chinese/work_tmp/determinism/a/         27 synthetic captures x 4 dumps = 135 files (the
#                                                 wide/narrow mix the engine's own kernels cover);
#   * doc_chinese/work_tmp/narrow_cmp2/<cap>/dev_*  20 REAL 1-2 PRB air captures x 4 dumps = 100 files
#                                                 (the S13-P2c corpus, device-built A).
#
# The dumps are named `<capture>_<slot>_<rnti>{,_ce.txt,_h.bin,_llr.bin}`, so the capture prefix plus a
# glob finds them; a dump that exists on one side only is an ERROR, never a pass.
#
# ---- Why it is a script rather than a loop typed once ----
# The loop was typed once, and it compared `_ce.txt` of ONE capture and reported "0 differences" for the
# other three files, because the glob matched a stale file from a previous capture (ab_dumps.sh's header
# records both failure modes). Here every comparison is per FILE, every file is counted, and a missing
# file fails the run.
#
# usage: bash neutral_vs_baseline.sh [tree]      (default: both corpora, current build)
# Exit: 0 all files identical; 1 a file differs; 2 a dump is missing / the build is absent.
set -u
ROOT=/Users/jiachengwang/dev/ocudu
BIN=${BIN:-$ROOT/build/lib/phy/upper/channel_processors/metal/ul_chain_replay}
WHAT=${1:-both}
[ -x "$BIN" ] || { echo "missing $BIN (build it first)" >&2; exit 2; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

nfiles=0
diffs=0
miss=0
badcaps=0

# compare <baseline-file> <new-file> <label>
compare()
{
  local base=$1 new=$2 label=$3
  if [ ! -f "$base" ]; then
    echo "  MISSING BASELINE: $label ($base)"
    miss=$((miss + 1))
    return
  fi
  if [ ! -f "$new" ]; then
    echo "  MISSING NEW DUMP: $label ($new)"
    miss=$((miss + 1))
    return
  fi
  nfiles=$((nfiles + 1))
  local k
  k=$(cmp -l "$base" "$new" 2>/dev/null | wc -l | tr -d ' ')
  if [ "$k" != "0" ]; then
    echo "  DIFFERENT: $label ($k bytes)"
    diffs=$((diffs + k))
  fi
}

# ---- corpus (27 captures, device-built A) ----
if [ "$WHAT" = "both" ] || [ "$WHAT" = "corpus" ]; then
  n=0
  cap_seen=0
  for cap in "$ROOT"/doc_chinese/work_tmp/corpus/*.bin; do
    name=$(basename "$cap" .bin)
    n=$((n + 1))
    out="$WORK/corpus/$name"
    mkdir -p "$WORK/corpus"
    "$BIN" "$ROOT/doc_chinese/work_tmp/corpus/$name" --metal --out "$out" >/dev/null 2>&1
    fresh=$(ls "$out"*_ce.txt 2>/dev/null | head -1)
    if [ -z "$fresh" ]; then
      echo "  NO DUMP: $name"
      miss=$((miss + 1))
      continue
    fi
    fresh=${fresh%_ce.txt}
    base=$(ls "$ROOT/doc_chinese/work_tmp/determinism/a/${name}_"*_ce.txt 2>/dev/null | head -1)
    if [ -z "$base" ]; then
      echo "  NO BASELINE: $name"
      miss=$((miss + 1))
      continue
    fi
    base=${base%_ce.txt}
    cap_seen=$((cap_seen + 1))
    before=$diffs
    for suf in .txt _ce.txt _h.bin _llr.bin .bin; do
      compare "$base$suf" "$fresh$suf" "$name$suf"
    done
    [ "$diffs" != "$before" ] && badcaps=$((badcaps + 1))
  done
  echo "[corpus] captures=$n compared=$cap_seen"
fi

# ---- the 20 real 1-2 PRB air captures (narrow_cmp2/<cap>/dev_*) ----
if [ "$WHAT" = "both" ] || [ "$WHAT" = "narrow" ]; then
  n=0
  cap_seen=0
  # *.txt, NOT *.bin: this directory holds the capture AND the leg's own result dumps, and the
  # latter's "_h.bin" matches a *.bin glob - which the first run of this script took for 20 more
  # captures and reported as 20 missing dumps (the guard worked, the glob was wrong).
  for cap in "$ROOT"/doc_chinese/work_tmp/narrow_cap/*.txt; do
    case "$cap" in *_ce.txt) continue ;; esac
    [ -e "$cap" ] || continue
    name=$(basename "$cap" .txt)
    n=$((n + 1))
    out="$WORK/narrow/$name"
    mkdir -p "$WORK/narrow"
    "$BIN" "$ROOT/doc_chinese/work_tmp/narrow_cap/$name" --metal --out "$out" >/dev/null 2>&1
    fresh=$(ls "$out"*_ce.txt 2>/dev/null | head -1)
    if [ -z "$fresh" ]; then
      echo "  NO DUMP: $name"
      miss=$((miss + 1))
      continue
    fi
    fresh=${fresh%_ce.txt}
    base=$(ls "$ROOT/doc_chinese/work_tmp/narrow_cmp2/$name/dev_"*_ce.txt 2>/dev/null | head -1)
    if [ -z "$base" ]; then
      echo "  NO BASELINE: $name"
      miss=$((miss + 1))
      continue
    fi
    base=${base%_ce.txt}
    cap_seen=$((cap_seen + 1))
    before=$diffs
    for suf in .txt _ce.txt _h.bin _llr.bin .bin; do
      compare "$base$suf" "$fresh$suf" "$name$suf"
    done
    [ "$diffs" != "$before" ] && badcaps=$((badcaps + 1))
  done
  echo "[narrow] captures=$n compared=$cap_seen"
fi

echo "files-compared=$nfiles differing-bytes=$diffs captures-with-differences=$badcaps missing=$miss"
if [ "$miss" != "0" ]; then
  echo "FAILED: $miss file(s) missing - this comparison is NOT evidence"
  exit 2
fi
[ "$diffs" = "0" ]
