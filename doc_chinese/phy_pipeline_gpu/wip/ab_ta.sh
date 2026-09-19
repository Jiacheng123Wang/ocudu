#!/usr/bin/env bash
# Batch 5b's offline A/B: the DEVICE time alignment against the HOST's, over the whole capture corpus.
#
# ---- What it compares, and why in this shape ----
#
# 5b moves the hop's timing advance off the host: the device places the pilots it finds in h, transforms
# them and reduces the power delay profile (K7 -> dft_dit -> K6, all in the reformat's own command
# buffer). Two things therefore have to hold, and they are NOT the same measurement:
#
#   1. nothing else moves. The TA is a REPORTING value - it feeds the timing-advance report and the
#      debug dump, never the LLR path - so `_llr.bin`, `_h.bin` and `.bin` must be byte-identical
#      between the two arms. That is what ab_dumps.sh checks, and this script checks it too, per
#      capture, so a run cannot pass on the average;
#   2. the value itself is the same one. The device and the host do not agree bit for bit (different
#      transforms, and the parabolic refinement is applied to each side's own output), so the criterion
#      is the RESOLUTION the transform size, the subcarrier spacing and the pilot stride set - which is
#      exactly what the OCUDU_CE_TA_CHECK probe compares in-process, on the same hop, with the host's
#      own estimator (`[ta_check] total: N checks, M outside one resolution`). This script reads that
#      line: it is a stronger comparison than diffing the two `_ce.txt` files, because it is the same
#      hop and not two runs of it.
#
# The `_ce.txt` files are compared as well, and the ONLY field allowed to differ is ta_us: a difference
# anywhere else means the arm changed something it was not supposed to (the rsrp comes from the device
# in both arms, so even that has to be identical).
#
# ---- Batch 5c: the same script also measures the flip of OCUDU_CE_HOST_GRID -----------------------
#
# Its default is 0 now (a hop whose reporting values the device produced is not read back), and the
# criterion for THAT is different in kind: nothing may move at all. So a third run per capture is made
# with OCUDU_CE_HOST_GRID=1 (the pre-5c behaviour) and every dump plus the whole `_ce.txt` must be
# byte-identical to the default arm - which is only true because the noise variance (K4) and the rsrp
# (K5) are published from the device too, not just the time alignment.
#
# usage: bash ab_ta.sh [corpus-glob]
# Exit: 0 when every capture produced both arms, the three dumps are byte-identical, no `_ce.txt` field
#       other than ta_us moved, the probe judged every hop inside one resolution, and the published
#       ta_us never differs by more than 0.2us (a bound above every resolution this port produces:
#       130ns for the narrowest hop, 65ns for 25 PRB).
set -u
ROOT=/Users/jiachengwang/dev/ocudu
GLOB=${1:-$ROOT/doc_chinese/work_tmp/corpus/*.bin}
NEW=$ROOT/build/lib/phy/upper/channel_processors/metal/ul_chain_replay

if [ ! -x "$NEW" ]; then echo "missing $NEW (build it first)" >&2; exit 2; fi
CAPTURES=$(ls $GLOB 2>/dev/null | sed -E 's/\.bin$//')
if [ -z "$CAPTURES" ]; then echo "no captures match $GLOB" >&2; exit 2; fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# The published value, out of the dump line: `... ta_us=-0.034587 cfo_hz=na`.
ta_of() { sed -nE 's/.* ta_us=([-0-9.eE+]+) .*/\1/p' "$1" | head -1; }
# The same line with the ta_us field removed, so every OTHER field can be compared byte for byte.
ce_without_ta() { sed -E 's/ ta_us=[-0-9.eE+]+//' "$1"; }

n=0; miss=0; bad_dump=0; bad_ce=0; bad_probe=0; worst_diff=0; bad_grid=0
for c in $CAPTURES; do
  n=$((n+1))
  name=$(basename "$c")
  dev="$WORK/dev$n"; host="$WORK/host$n"; grid="$WORK/grid$n"
  mkdir -p "$dev" "$host" "$grid"
  # Arm A: the device TA with the in-process A/B probe on. It also pins OCUDU_CE_HOST_GRID=1: the probe
  # runs the HOST's estimator on the same hop, which needs the host grid (and that is also the pre-5c
  # behaviour arm C is compared against).
  OCUDU_CE_HOST_GRID=1 OCUDU_CE_TA_CHECK=1 "$NEW" "$c" --out "$dev/d" --metal >"$dev/log" 2>&1
  # Arm B: the host's own estimator, which is what the port published before 5b. It needs no
  # HOST_GRID: a hop the device did not cover for the TA keeps its read-back by construction.
  OCUDU_CE_DEV_TA=0 "$NEW" "$c" --out "$host/d" --metal >"$host/log" 2>&1
  # Arm C: the SHIPPED DEFAULT (batch 5c), i.e. no OCUDU_CE_HOST_GRID: the hop's reporting values come
  # from the device and the grid is not unpacked. Everything but the crossing count must be identical to
  # arm A.
  "$NEW" "$c" --out "$grid"/d --metal >"$grid/log" 2>&1

  da=$(ls "$dev"/d*_ce.txt 2>/dev/null | head -1)
  hb=$(ls "$host"/d*_ce.txt 2>/dev/null | head -1)
  if [ -z "$da" ] || [ -z "$hb" ]; then
    echo "  MISSING DUMP: $name  device=$([ -n "$da" ] && echo ok || echo NONE)  host=$([ -n "$hb" ] && echo ok || echo NONE)"
    miss=$((miss+1))
    continue
  fi

  # (1) nothing else moves.
  for suf in _llr.bin _h.bin .bin; do
    k=$(cmp -l "${da%_ce.txt}$suf" "${hb%_ce.txt}$suf" 2>/dev/null | wc -l | tr -d ' ')
    if [ "$k" != "0" ]; then
      echo "  DUMP DIFFERS: $name$suf ($k bytes)"
      bad_dump=$((bad_dump+1))
    fi
  done
  if ! diff <(ce_without_ta "$da") <(ce_without_ta "$hb") >/dev/null; then
    echo "  _ce.txt DIFFERS OUTSIDE ta_us: $name"
    echo "    device: $(cat "$da")"
    echo "    host  : $(cat "$hb")"
    bad_ce=$((bad_ce+1))
  fi

  # (2) the value itself: the probe's verdict on the hops of this capture...
  probe=$(grep -E '^\[ta_check\] total:' "$dev/log" | tail -1)
  if [ -z "$probe" ]; then
    echo "  NO PROBE VERDICT: $name (the device TA never ran, or the grid was not published)"
    bad_probe=$((bad_probe+1))
  else
    outside=$(echo "$probe" | sed -nE 's/.* ([0-9]+) outside one resolution.*/\1/p')
    checks=$(echo "$probe" | sed -nE 's/.*total: ([0-9]+) checks.*/\1/p')
    if [ "${outside:-x}" != "0" ] || [ "${checks:-0}" = "0" ]; then
      echo "  PROBE FAILED: $name -> $probe"
      bad_probe=$((bad_probe+1))
    fi
  fi

  # (3) batch 5c: the shipped default must change NOTHING but the crossings.
  dg=$(ls "$grid"/d*_ce.txt 2>/dev/null | head -1)
  if [ -z "$dg" ]; then
    echo "  MISSING DUMP (HOST_GRID=1 arm): $name"
    bad_grid=$((bad_grid+1))
  else
    for suf in _llr.bin _h.bin .bin _ce.txt; do
      k=$(cmp -l "${da%_ce.txt}$suf" "${dg%_ce.txt}$suf" 2>/dev/null | wc -l | tr -d ' ')
      if [ "$k" != "0" ]; then
        echo "  HOST_GRID CHANGED $suf: $name ($k bytes)"
        bad_grid=$((bad_grid+1))
      fi
    done
  fi

  # ... and the two runs' published values, which is the same statement one level up.
  tadev=$(ta_of "$da"); tahost=$(ta_of "$hb")
  diff_us=$(awk -v a="$tadev" -v b="$tahost" 'BEGIN { d = a - b; if (d < 0) d = -d; printf "%.6f", d }')
  worst_diff=$(awk -v w="$worst_diff" -v d="$diff_us" 'BEGIN { print (d > w) ? d : w }')
  # The crossings of both arms, which is what batch 5c is FOR: the read side must fall.
  cr_grid=$(grep -oE "= [0-9.]+ read\(s\) \+ [0-9.]+ write\(s\) per hop" "$grid/log" | tail -1)
  cr_dev=$(grep -oE "= [0-9.]+ read\(s\) \+ [0-9.]+ write\(s\) per hop" "$dev/log" | tail -1)
  printf "  %-14s ta_us device %+.6f  host %+.6f  |diff| %.6f us%s\n" \
         "$name" "$tadev" "$tahost" "$diff_us" "$(awk -v d="$diff_us" 'BEGIN { print (d > 0.2) ? "  <-- OVER 0.2us" : "" }')"
  printf "  %-14s crossings: pre-5c%s | shipped default%s\n" "" "${cr_dev:- ?}" "${cr_grid:- ?}"
done

echo "captures=$n  missing-dumps=$miss  dump-differences=$bad_dump  ce-differences-outside-ta=$bad_ce  probe-failures=$bad_probe  host-grid-differences=$bad_grid"
echo "worst |ta_us difference| = $worst_diff us (every resolution this port produces is below 0.14us)"
if [ "$miss" != "0" ] || [ "$bad_dump" != "0" ] || [ "$bad_ce" != "0" ] || [ "$bad_probe" != "0" ] || [ "$bad_grid" != "0" ]; then
  echo "FAILED: this comparison is NOT evidence of a clean port"
  exit 2
fi
awk -v w="$worst_diff" 'BEGIN { exit (w > 0.2) ? 1 : 0 }' || { echo "FAILED: the published value moved by more than a resolution"; exit 1; }
echo "PASSED: the device time alignment is the host's to within a resolution, and neither the TA"
echo "        switch nor the OCUDU_CE_HOST_GRID flip (batch 5c) moved a single published byte"
