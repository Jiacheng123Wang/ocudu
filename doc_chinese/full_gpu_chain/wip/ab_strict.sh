#!/usr/bin/env bash
# Strict cross-binary A/B: every published dump must be byte-identical to the reference binary's.
#
# This is the sensitive net for the whole upper-PHY chain, and the one the host-LS route keeps usable
# even now that the device route reduces the pilots' mean power on the device (see the design doc,
# 48.164): with OCUDU_CE_CPU_LS=1 the estimator stages its pilots on the host and nothing in the
# published values depends on the device-side summation order.
#
# usage: bash ab_strict.sh <reference-binary> [nof-captures] [env knob] [mode]
#   mode defaults to --metal; pass --cpu to check the pure CPU chain (no knob needed).
#
# THE REFERENCE MUST BE REBUILT ON THIS MACHINE (design doc 48.176(d)): the same commit built on a
# different day or toolchain differs by ~1 ULP in the _ce.txt noise_variance/rsrp scalars, and that
# alone turns this net red while every llr/h/grid byte stays identical - a stale artifact, not a
# regression. Check that fingerprint before believing a red strict net.
set -u
REF=${1:?reference binary, e.g. doc_chinese/work_tmp/ref/ul_chain_replay_s7g5_ref (durable) or /tmp/...}
N=${2:-30}
KNOB=${3:-NONE}
MODE=${4:---metal}
NEW=/Users/jiachengwang/dev/ocudu/build/lib/phy/upper/channel_processors/metal/ul_chain_replay
CORPUS=${CORPUS:-$( [ -d /Users/jiachengwang/dev/ocudu/doc_chinese/work_tmp/corpus ] && echo /Users/jiachengwang/dev/ocudu/doc_chinese/work_tmp/corpus || echo /tmp/corpus )}

# A missing or empty corpus is an ERROR, never a pass: a comparison over nothing is vacuously
# identical, and /tmp does not survive a reboot.
CAPTURES=$(ls "$CORPUS"/*.bin 2>/dev/null | sed -E 's/\.bin$//' | head -"$N")
if [ -z "$CAPTURES" ]; then echo "no captures in $CORPUS: nothing was compared" >&2; exit 2; fi

same=0; n=0; dif=""
for c in $CAPTURES; do
  n=$((n+1)); rm -f /tmp/strict0* /tmp/strict1*
  if [ "$KNOB" = NONE ]; then
    "$REF" "$c" $MODE --out /tmp/strict0 >/dev/null 2>&1
    "$NEW" "$c" $MODE --out /tmp/strict1 >/dev/null 2>&1
  else
    env "$KNOB" "$REF" "$c" $MODE --out /tmp/strict0 >/dev/null 2>&1
    env "$KNOB" "$NEW" "$c" $MODE --out /tmp/strict1 >/dev/null 2>&1
  fi
  a=$(ls /tmp/strict0*_ce.txt 2>/dev/null | head -1); b=$(ls /tmp/strict1*_ce.txt 2>/dev/null | head -1)
  if [ -z "$a" ] || [ -z "$b" ]; then dif="$dif $(basename "$c")(nodump)"; continue; fi
  ok=1
  for suf in _llr.bin _ce.txt _h.bin .bin; do
    cmp -s "${a%_ce.txt}$suf" "${b%_ce.txt}$suf" || ok=0
  done
  if [ "$ok" = 1 ]; then same=$((same+1)); else dif="$dif $(basename "$c")"; fi
done
echo "strict A/B ref=$(basename "$REF") mode=$MODE knob=$KNOB: llr+ce+h+grid identical $same/$n$dif"
[ "$same" -eq "$n" ] || exit 1
