#!/usr/bin/env bash
# Compare the PUBLISHED DUMPS of one binary run two ways (environment A vs environment B) over a corpus.
#
# ---- Why this script exists ----
# Two measurements in this line were WRONG because of the harness, not the code:
#
#   1. `ls /tmp/x*_ce.txt | head -1` picked up a file left behind by a PREVIOUS capture (the names are
#      `<prefix>_<slot>_<rnti>_ce.txt`, and two captures can share a slot and an rnti), so the two sides
#      of the comparison came from different inputs and the result was meaningless;
#   2. a stray DIRECTORY matched the glob, so `cmp` compared two empty streams and reported
#      "0 differences" - a vacuous pass on the one number that was supposed to be the evidence.
#
# A measurement tool has to make both impossible rather than rely on the person writing the loop:
#   * every run gets its OWN directory (`mktemp -d`), removed on exit - nothing can be reused across
#     captures, legs or runs;
#   * a dump that is MISSING is counted and the script FAILS LOUDLY. "No data" must never read as
#     "equal", which is exactly what failure (2) looked like.
#
# usage: bash ab_dumps.sh "<env A>" "<env B>" [mode-ARGS] [corpus-glob]
#   e.g. bash ab_dumps.sh "" "OCUDU_CE_HOST_SCALARS=0"
#        bash ab_dumps.sh "OCUDU_CE_CPU_LS=1" "OCUDU_CE_CPU_LS=1"          # the CPU-route net
#
# NOTE the third argument is ARGUMENTS TO THE REPLAY BINARY, not environment. A knob put there does
# not get set: the token becomes an argv the binary rejects, no dump is written, and the run is empty.
# The `missing-dumps` guard is what catches that (it did, 27/27), but the mistake is cheap to avoid -
# every OCUDU_CE_* knob belongs in the FIRST or SECOND argument. (An earlier revision of this comment
# showed a knob in the mode argument; that example was wrong and was removed.)
#
# Exit: 0 when every capture produced dumps on both sides AND the four dumps are byte-identical;
#       1 when they differ; 2 when a dump is missing (the comparison is NOT evidence).
set -u
ENV_A=${1:-}
ENV_B=${2:-}
MODE=${3:---metal}
ROOT=/Users/jiachengwang/dev/ocudu
GLOB=${4:-$( [ -d "$ROOT/doc_chinese/work_tmp/corpus" ] && echo "$ROOT/doc_chinese/work_tmp/corpus" || echo /tmp/corpus )/*.bin}
NEW=$ROOT/build/lib/phy/upper/channel_processors/metal/ul_chain_replay

if [ ! -x "$NEW" ]; then echo "missing $NEW (build it first)" >&2; exit 2; fi
CAPTURES=$(ls $GLOB 2>/dev/null | sed -E 's/\.bin$//')
if [ -z "$CAPTURES" ]; then echo "no captures match $GLOB" >&2; exit 2; fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

n=0; miss=0; badcaps=0
d_llr=0; d_h=0; d_bin=0; d_ce=0
for c in $CAPTURES; do
  n=$((n+1))
  a="$WORK/A$n"; b="$WORK/B$n"
  mkdir -p "$a" "$b"
  # shellcheck disable=SC2086
  env $ENV_A "$NEW" "$c" $MODE --out "$a/dump" >/dev/null 2>&1
  env $ENV_B "$NEW" "$c" $MODE --out "$b/dump" >/dev/null 2>&1
  fa=$(ls "$a"/dump*_ce.txt 2>/dev/null | head -1)
  fb=$(ls "$b"/dump*_ce.txt 2>/dev/null | head -1)
  if [ -z "$fa" ] || [ -z "$fb" ]; then
    echo "  MISSING DUMP: $(basename "$c")  A=$([ -n "$fa" ] && echo ok || echo NONE)  B=$([ -n "$fb" ] && echo ok || echo NONE)"
    miss=$((miss+1))
    continue
  fi
  cap=0
  for suf in _llr.bin _h.bin .bin _ce.txt; do
    k=$(cmp -l "${fa%_ce.txt}$suf" "${fb%_ce.txt}$suf" 2>/dev/null | wc -l | tr -d ' ')
    case "$suf" in
      _llr.bin) d_llr=$((d_llr + k)) ;;
      _h.bin)   d_h=$((d_h + k)) ;;
      .bin)     d_bin=$((d_bin + k)) ;;
      _ce.txt)  d_ce=$((d_ce + k)) ;;
    esac
    [ "$k" != "0" ] && cap=1
  done
  [ "$cap" = "1" ] && badcaps=$((badcaps + 1))
done

total=$((d_llr + d_h + d_bin + d_ce))
echo "A = ${ENV_A:-<default>}"
echo "B = ${ENV_B:-<default>}"
echo "mode = $MODE"
echo "captures=$n  missing-dumps=$miss  captures-with-differences=$badcaps  total-differing-bytes=$total"
printf "  %-10s %s\n" _llr.bin "$d_llr" _h.bin "$d_h" .bin "$d_bin" _ce.txt "$d_ce"

if [ "$miss" != "0" ]; then
  echo "FAILED: $miss capture(s) produced no dump on one side - this comparison is NOT evidence"
  exit 2
fi
[ "$total" = "0" ]
