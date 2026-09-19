#!/usr/bin/env bash
# Bounded-difference A/B: for the one path where byte identity cannot hold.
#
# The estimator's pilots' mean power is reduced on the device now (see mmse_pilots_power), so its
# summation order differs from the host's: the published dumps change in the last bits of some
# samples. Byte identity is therefore not the right gate for this path - but an unbounded "anything
# goes" is not a gate at all, so this script bounds the difference and checks that no DECISION
# changed:
#   * per capture: how many bytes differ in each dump, and the largest LLR byte delta
#   * per capture: whether the LLR SIGN pattern (the hard decision of every soft bit) is identical
# Bounds (from the measured sensitivity of the dumps to sigma2_rel, see the design doc):
#   differing captures   <= 10/30  (a deliberate 1e-7 perturbation already moves 5/30, 1e-6 moves 10/30)
#   differing bytes      <= 1% of a dump (measured worst: 159 bytes of 33600 = 0.47%; a semantic
#                                      error moves whole blocks, not half a percent of the values)
#   LLR decision flips   == 0      (the semantic gate: no soft bit may change sign)
set -u
REF=$1
NEW=/Users/jiachengwang/dev/ocudu/build/lib/phy/upper/channel_processors/metal/ul_chain_replay
CORPUS=${CORPUS:-$( [ -d /Users/jiachengwang/dev/ocudu/doc_chinese/work_tmp/corpus ] && echo /Users/jiachengwang/dev/ocudu/doc_chinese/work_tmp/corpus || echo /tmp/corpus )}
N=${2:-30}
MODES=${3:---metal}
rc_all=0
for MODE in $MODES; do
same=0; n=0; worst_bytes=0; worst_cap=""; flips=0; dif=""
# The corpus is a directory of capture prefixes (<name>.txt + <name>.bin). A missing or empty
# corpus is an ERROR, never a pass: a comparison over nothing is vacuously identical.
CAPTURES=$(ls "$CORPUS"/*.bin 2>/dev/null | sed -E 's/\.bin$//' | head -"$N")
if [ -z "$CAPTURES" ]; then echo "no captures in $CORPUS: nothing was compared" >&2; exit 2; fi
for c in $CAPTURES; do
  n=$((n+1)); rm -f /tmp/t0* /tmp/t1*
  "$REF" "$c" $MODE --out /tmp/t0 >/dev/null 2>&1
  "$NEW" "$c" $MODE --out /tmp/t1 >/dev/null 2>&1
  a=$(ls /tmp/t0*_ce.txt 2>/dev/null | head -1); b=$(ls /tmp/t1*_ce.txt 2>/dev/null | head -1)
  if [ -z "$a" ] || [ -z "$b" ]; then dif="$dif $(basename "$c")(nodump)"; continue; fi
  tot=0
  for suf in _llr.bin _h.bin .bin; do
    d=$(cmp -l "${a%_ce.txt}$suf" "${b%_ce.txt}$suf" 2>/dev/null | wc -l | tr -d ' ')
    tot=$((tot + d))
  done
  cerr=$(cmp -l "$a" "$b" 2>/dev/null | wc -l | tr -d ' ')
  tot=$((tot + cerr))
  # LLR decisions: the sign of every soft bit must be untouched.
  f=$(python3 - "$a" "$b" <<'PY'
import sys
pa = sys.argv[1].replace("_ce.txt", "_llr.bin")
pb = sys.argv[2].replace("_ce.txt", "_llr.bin")
try:
    A = open(pa, "rb").read()
    B = open(pb, "rb").read()
except OSError:
    print(-1); raise SystemExit
if len(A) != len(B):
    print(-1); raise SystemExit
# The dump holds signed 8-bit LLRs; a decision is the sign bit.
flips = sum(1 for x, y in zip(A, B) if ((x ^ y) & 0x80) != 0)
print(flips)
PY
)
  [ "$f" = "0" ] || flips=$((flips + (f < 0 ? 1 : f)))
  if [ "$tot" = "0" ]; then same=$((same+1)); else dif="$dif $(basename "$c")"; fi
  if [ "$tot" -gt "$worst_bytes" ]; then worst_bytes=$tot; worst_cap=$(basename "$c"); fi
done
echo "bounded A/B mode=$MODE: byte-identical $same/$n; differing bytes worst=$worst_bytes ($worst_cap); LLR decision flips=$flips"
rc=0
[ "$((n - same))" -le 10 ] || rc=1
[ "$worst_bytes" -le 336 ] || rc=1
[ "$flips" -eq 0 ] || rc=1
[ "$rc" -eq 0 ] || rc_all=1
done
echo "BOUNDED_AB_RC=$rc_all (bounds per mode: differing captures <=10, bytes <=1% of a dump, flips 0)"
exit $rc_all
