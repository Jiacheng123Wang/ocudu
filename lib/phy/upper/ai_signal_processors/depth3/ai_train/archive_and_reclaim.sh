#!/usr/bin/env bash
# ARCHIVE A LEG'S RAW CAPTURES, THEN DELETE THEM, BECAUSE THE DUMP ALREADY HOLDS WHAT THE DATASETS NEED.
#
# WHY THIS IS SAFE. A leg's raw captures are the largest thing on disk (twenty gigabytes and up) and the cheapest to replace, because the dump
# (tb_<leg>/) already holds, per unit, the decoded transport block, the recovered ones, and the index that points back at the capture. The
# datasets are built from that index and the capture's sidecars, and the sidecars are inside the archive. So a leg that has been dumped can be
# deleted from the working tree and rebuilt later from its .tar, while the dump -- three to five percent of the capture size -- stays.
#
# WHY IT VERIFIES BEFORE DELETING. A half-written tar that is mistaken for complete is silent data loss, and the deletion that follows it is not.
# Each archive is written to <leg>.tar.part, tar must exit zero, the entry count must equal the source tree's, and only then is it renamed and
# the source removed. With --delete the deletion happens per leg, right after that leg's archive is proven.
#
# Usage: archive_and_reclaim.sh --dest <dir> [--corpus DIR] [--tb-root DIR] [--delete] [--dry-run] [leg ...]
#        (no legs: every tb_<leg> that has an index.csv, i.e. every leg that has been dumped)
set -u
DEST=""
CORPUS=${CORPUS:-$HOME/work/ai_phy_train/corpus}
TB_ROOT=${TB_ROOT:-$HOME/work/ai_phy_train}
DO_DELETE=0
DRY=0
LEGS=()

while [ $# -gt 0 ]; do
  case "$1" in
    --dest)    DEST=$2; shift 2 ;;
    --corpus)  CORPUS=$2; shift 2 ;;
    --tb-root) TB_ROOT=$2; shift 2 ;;
    --delete)  DO_DELETE=1; shift ;;
    --dry-run) DRY=1; shift ;;
    *)         LEGS+=("$1"); shift ;;
  esac
done
[ -n "$DEST" ] || { echo "usage: $0 --dest <dir> [--delete] [--dry-run] [leg ...]" >&2; exit 2; }
mkdir -p "$DEST" || exit 2

if [ ${#LEGS[@]} -eq 0 ]; then
  for d in "$TB_ROOT"/tb_*/; do
    leg=$(basename "$d"); leg=${leg#tb_}
    [ -s "$TB_ROOT/tb_$leg/index.csv" ] && LEGS+=("$leg")
  done
fi

freed_kb=0
for leg in "${LEGS[@]}"; do
  src=$CORPUS/$leg
  [ -d "$src" ] || { echo "skip $leg (no captures here)"; continue; }
  if [ ! -s "$TB_ROOT/tb_$leg/index.csv" ]; then
    echo "REFUSE $leg: no tb_$leg/index.csv, so nothing would be left to rebuild from." >&2
    continue
  fi
  size_kb=$(du -sk "$src" | awk '{print $1}')
  if [ -f "$DEST/$leg.tar" ]; then
    echo "have $leg.tar ($(du -h "$DEST/$leg.tar" | cut -f1))"
  else
    echo "---- archiving $leg ($(du -sh "$src" | cut -f1)) $(date +%T)"
    if [ "$DRY" -eq 1 ]; then echo "     (dry-run)"; continue; fi
    if ! tar -cf "$DEST/$leg.tar.part" -C "$CORPUS" "$leg"; then
      echo "REFUSE $leg: tar failed, kept as .part and nothing deleted" >&2; continue
    fi
    want=$(find "$src" | wc -l | tr -d ' ')
    got=$(tar -tf "$DEST/$leg.tar.part" 2>/dev/null | wc -l | tr -d ' ')
    if [ "$want" != "$got" ]; then
      echo "REFUSE $leg: archive holds $got of $want entries, kept as .part and nothing deleted" >&2; continue
    fi
    mv "$DEST/$leg.tar.part" "$DEST/$leg.tar"
    echo "     verified $got entries, $(du -h "$DEST/$leg.tar" | cut -f1)"
  fi
  if [ "$DO_DELETE" -eq 1 ]; then
    rm -rf "$src" && echo "     deleted $src, freed $(du -h --apparent-size "$DEST/$leg.tar" | cut -f1) of captures (disk: $((size_kb/1024)) MB)"
    freed_kb=$((freed_kb + size_kb))
  fi
done
[ "$DO_DELETE" -eq 1 ] && echo "===== reclaimed $((freed_kb/1024/1024)) GB of raw captures"
echo "===== done $(date +%T)"
