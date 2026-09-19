#!/usr/bin/env bash
# CPU-side A/B: reference binary vs current build, over the PURE CPU chain (--phy_pipeline cpu: the
# historical default path, every module on the CPU).
#
# NOT here: the two module-level mixes of --phy_pipeline cpu_gpu (--metal-cpu-ldpc, --metal-cpu-demod)
# run the Metal channel estimator, so they belong to the device route and its bounded gate
# (wip/ab_tol.sh) - byte identity cannot be expected of them since the estimator reduces the pilots'
# mean power on the device (see the design doc, 48.164).
#
# Why this exists separately from ab_ref.sh (which runs --metal): the GPU path is an ALTERNATIVE
# path on Apple Silicon, not a replacement. The original CPU path has to keep producing exactly
# what it produced before, and the metal A/B cannot see it (it exercises the device backends on
# both binaries, so a CPU-side regression stays invisible there).
#
# usage: bash ab_cpu.sh <reference-binary> [nof-captures] [env knob]
#   reference-binary: e.g. /tmp/ul_chain_replay_s7f6d_ref (the leg before the change under test)
set -u
REF=$1
N=${2:-30}
KNOB=${3:-NONE}
NEW=/Users/jiachengwang/dev/ocudu/build/lib/phy/upper/channel_processors/metal/ul_chain_replay
CORPUS=${CORPUS:-$( [ -d /Users/jiachengwang/dev/ocudu/doc_chinese/work_tmp/corpus ] && echo /Users/jiachengwang/dev/ocudu/doc_chinese/work_tmp/corpus || echo /tmp/corpus )}
rc_all=0
for mode in --cpu; do
  same=0; n=0; dif=""; empty=0
  # The corpus is a directory of capture prefixes (<name>.txt + <name>.bin). A missing or empty
# corpus is an ERROR, never a pass: a comparison over nothing is vacuously identical.
CAPTURES=$(ls "$CORPUS"/*.bin 2>/dev/null | sed -E 's/\.bin$//' | head -"$N")
if [ -z "$CAPTURES" ]; then echo "no captures in $CORPUS: nothing was compared" >&2; exit 2; fi
for c in $CAPTURES; do
    n=$((n+1)); rm -f /tmp/abcr_* /tmp/abcn_*
    if [ "$KNOB" = NONE ]; then
      "$REF" "$c" "$mode" --out /tmp/abcr >/dev/null 2>&1
      "$NEW" "$c" "$mode" --out /tmp/abcn >/dev/null 2>&1
    else
      env "$KNOB" "$REF" "$c" "$mode" --out /tmp/abcr >/dev/null 2>&1
      env "$KNOB" "$NEW" "$c" "$mode" --out /tmp/abcn >/dev/null 2>&1
    fi
    a=$(ls /tmp/abcr_*_ce.txt 2>/dev/null | head -1); b=$(ls /tmp/abcn_*_ce.txt 2>/dev/null | head -1)
    if [ -z "$a" ] || [ -z "$b" ]; then dif="$dif $(basename "$c")(nodump)"; continue; fi
    # A comparison of empty dumps would be vacuously identical: require a non-empty LLR dump.
    if [ ! -s "${a%_ce.txt}_llr.bin" ] || [ ! -s "${b%_ce.txt}_llr.bin" ]; then
      empty=$((empty+1)); dif="$dif $(basename "$c")(empty-llr)"; continue
    fi
    ok=1
    cmp -s "${a%_ce.txt}_llr.bin" "${b%_ce.txt}_llr.bin" || ok=0
    cmp -s "$a" "$b" || ok=0
    cmp -s "${a%_ce.txt}_h.bin" "${b%_ce.txt}_h.bin" || ok=0
    cmp -s "${a%_ce.txt}.bin" "${b%_ce.txt}.bin" || ok=0
    if [ "$ok" = 1 ]; then same=$((same+1)); else dif="$dif $(basename "$c")"; fi
  done
  echo "CPU-side ref=$(basename "$REF") mode=$mode knob=$KNOB: llr+ce+h+grid identical $same/$n (empty=$empty)$dif"
  [ "$same" -eq "$n" ] || rc_all=1
done
echo "CPU_AB_RC=$rc_all"
exit $rc_all
