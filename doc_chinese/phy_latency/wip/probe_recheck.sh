#!/usr/bin/env bash
# The four readings the post-probe-change leg pair owes, each JUDGED against what was pre-registered
# before the legs were flown (dev doc 6.147 (4)), next to the p86 baseline they are compared with.
#
# WHY IT EXISTS. The pair is re-flown after a PROBE change, so its acceptance is not only "the delivery
# criteria still hold" (that is milestone_audit.sh's job) but also "the new instrument did what it was
# pre-registered to do, AND did not perturb what it measures". Those are two different questions and a
# leg can pass the first while failing the second, which is exactly why they are read side by side here:
#
#   1. THE PRE-REGISTERED NUMBERS. `[ul_rx_wait] max` used to be a constant ~101 ms on every leg - the
#      start-up sample that 6.146 explained and 6.147 stripped out. The prediction was MAX <= 20 ms;
#      if the max is still ~101 ms the strip did not take effect and THE LEG IS VOID for this line
#      (not a regression, an instrument that did not run). The same call must now appear ONCE as its
#      own field (`startup=`), and the new hop-scoped series must track the per-block one.
#   2. THE GPU-MODE SLOT TIMELINE. 6.145 (4) concluded from the code that trace_slot() is called
#      unconditionally at four landmarks and all four are reached in the fused lane, so
#      `[ul_slot_trace]` works in mode=gpu unchanged - a reading that had never been flown. What it
#      adds is the pair of instants no series has: the arrival of the samples that COMPLETE a slot and
#      the HOST-side landmarks after it (t2f/ce are host instants in this mode; the device ones are
#      [ul_gpu_lane]'s).
#   3. THE PERTURBATION SELF-CHECK. Both new instruments cost host work on paths this leg measures:
#      record_slot_rx_wait() one bounded map insert per completed slot on the RECEIVE thread,
#      record_slot_samples_complete()/trace_slot() more when the trace is on. So the leg has to show
#      [ul_rx_timing]'s loop/slip at the p86 level, crossings at 0 and the contract at 9 of 9. A probe
#      that moves what it measures is void (6.145 (3)).
#   4. V1 NEXT TO THE p86 BASELINE, so that a delivery regression cannot hide behind the new knobs.
#
# usage: bash probe_recheck.sh <leg .stderr | leg label> [<leg .stderr | leg label> ...]
#        (a bare label is looked up in wip/logs the way ul_load.sh does it: the newest leg carrying it)
set -u

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
LOGDIR=$ROOT/doc_chinese/phy_pipeline_gpu/wip/logs

# --- the p86 baseline, read off that leg's own stderr (kept HERE with its source, because a comparison
#     without the reference values written down is how "unchanged" becomes unfalsifiable) --------------
REF_LEG=p86-n78-stress
REF_RXWAIT_MED=473.0     # [ul_rx_wait] median, us
REF_RXWAIT_MAX=100786.0  # [ul_rx_wait] max, us  <- the start-up sample, still inside the distribution
REF_LOOP_MAX=1644        # [ul_rx_timing] loop max, us
REF_SLIP_MAX=8820        # [ul_rx_timing] slip max, us
REF_V1_MED=1409.8        # [ul_gpu_pipeline] median, us (V1's criterion is <= 2150)

# Pre-registered bounds (6.147 (4)) - NOT derived from the new legs.
MAX_AFTER_STRIP=20000    # us; the strip's prediction was 5-20 ms (p86 had 9 calls over 5 ms)
STARTUP_LO=90000         # us; every one of the 53 legs with [ul_rx_timing] read 100.5-101.9 ms
STARTUP_HI=130000
HOP_TOL_PCT=25           # [ul_rx_wait_hop] median vs [ul_rx_wait] median, whole-slot policy
V1_CRIT=2150

RESOLVED=()
for a in "$@"; do
  if [ -f "$a" ]; then RESOLVED+=("$a"); continue; fi
  f=$(ls -t "$LOGDIR"/*"$a"*.log.stderr 2>/dev/null | head -1)
  if [ -z "$f" ]; then echo "no leg matches '$a' under $LOGDIR" >&2; exit 2; fi
  RESOLVED+=("$f")
done
[ ${#RESOLVED[@]} -gt 0 ] || { echo "usage: bash $0 <leg .stderr | leg label> [...]" >&2; exit 2; }

n_fail=0
judge() { # <what> <expected> <got> <verdict>
  printf '  %-46s %-22s %-24s %s\n' "$1" "$2" "$3" "$4"
  [ "$4" = "FAIL" ] && n_fail=$((n_fail+1))
  return 0
}

for f in "${RESOLVED[@]}"; do
  leg=$(basename "$f" .log.stderr | sed 's/^gnb_[a-z_]*_//')
  echo
  echo "================ $leg ================"
  printf '  regime=%s  mode=%s  cell=%s\n' \
    "$(grep -aoE '^\[leg\] regime=[a-z]+' "$f" | head -1 | sed 's/.*=//')" \
    "$(grep -aoE '^pipeline mode : .*' "$f" | head -1 | sed 's/.*: //')" \
    "$(grep -aoE '^cell config   : .*' "$f" | head -1 | sed 's/.*: //')"
  grep -aE '^knob *: ' "$f" | sed 's/^/    /'

  echo "--- (1) the pre-registered numbers ------------------------------------------------------------------"
  rx=$(grep -aE '^\[ul_rx_wait\] samples=' "$f" | tail -1)
  if [ -z "$rx" ]; then
    judge "[ul_rx_wait] is readable" "a samples= line" "<none>" FAIL
  else
    rx_max=$(echo "$rx" | sed 's/.*max=\([0-9.]*\)us.*/\1/')
    rx_med=$(echo "$rx" | sed 's/.*median=\([0-9.]*\)us.*/\1/')
    v=$(awk -v m="$rx_max" -v b="$MAX_AFTER_STRIP" 'BEGIN{print (m<=b)?"PASS":"FAIL"}')
    judge "[ul_rx_wait] max after the strip" "<= ${MAX_AFTER_STRIP}us (was ${REF_RXWAIT_MAX})" "${rx_max}us" "$v"
    echo "      $rx"
  fi
  st=$(grep -aE '^\[ul_rx_wait\] startup=' "$f" | tail -1)
  if [ -z "$st" ]; then
    judge "[ul_rx_wait] startup= reported apart" "one startup= line" "<absent>" FAIL
  else
    st_us=$(echo "$st" | sed 's/.*startup=\([0-9.]*\)us.*/\1/')
    v=$(awk -v x="$st_us" -v lo="$STARTUP_LO" -v hi="$STARTUP_HI" 'BEGIN{print (x>=lo && x<=hi)?"PASS":"FAIL"}')
    judge "[ul_rx_wait] startup= value" "${STARTUP_LO}-${STARTUP_HI}us (53-leg band)" "${st_us}us" "$v"
  fi
  hop=$(grep -aE '^\[ul_rx_wait_hop\] samples=' "$f" | tail -1)
  if [ -z "$hop" ]; then
    judge "[ul_rx_wait_hop] is readable" "a samples= line" "<none: series absent>" FAIL
  else
    hop_med=$(echo "$hop" | sed 's/.*median=\([0-9.]*\)us.*/\1/')
    v=$(awk -v h="$hop_med" -v r="$rx_med" -v t="$HOP_TOL_PCT" \
        'BEGIN{d=(h>r)?(h-r):(r-h); print ((r>0) && (100*d/r<=t))?"PASS":"FAIL"}')
    judge "[ul_rx_wait_hop] median tracks [ul_rx_wait]" "within ${HOP_TOL_PCT}% of ${rx_med}us" "${hop_med}us" "$v"
    echo "      $hop"
    blk=$(echo "$rx" | sed 's/.*samples=\([0-9]*\).*/\1/'); hp=$(echo "$hop" | sed 's/.*samples=\([0-9]*\).*/\1/')
    printf '      hop/block sample ratio: %s/%s = %s  (INFO: a property of the TRAFFIC, not of the probe;\n' \
      "$hp" "$blk" "$(awk -v a="$hp" -v b="$blk" 'BEGIN{printf (b>0)?"%.3f":"n/a", (b>0)?a/b:0}')"
    printf '        whole-slot legs read ~0.26 on p86 - one hop per ~3.9 slots, see 6.123 (3))\n'
  fi

  echo "--- (2) the GPU-mode per-slot timeline ([ul_slot_trace]) -------------------------------------------"
  if ! grep -aq 'OCUDU_UL_SLOT_TRACE' "$f"; then
    echo "      <the switch was not on: nothing to read here>"
  else
    grep -aE '^\[ul_slot_trace\]' "$f" | head -2 | sed 's/^/      /'
    grep -aE '^  *[0-9]+ +[0-9]' "$f" | head -6 | sed 's/^/      /'
    rows=$(grep -acE '^  *[0-9]+ +[0-9]' "$f")
    judge "[ul_slot_trace] rows with a timeline" ">= 1 (bound is the switch value)" "$rows" \
          "$([ "$rows" -ge 1 ] && echo PASS || echo FAIL)"
    # The columns are: slot rxwait t2f ce ldpc crc_ok tf_from_done pipeline. In mode=gpu t2f/ce are HOST
    # instants (see 6.145 (4)); what must NOT happen is a column that is NaN for every row, which is what a
    # landmark that is never reached in this mode would produce.
    nan=$(grep -aE '^  *[0-9]+ +[0-9]' "$f" | grep -c 'nan' || true)
    judge "[ul_slot_trace] rows carrying a NaN column" "INFO (a failed decode has no crc_ok)" "$nan" INFO
    # THE FIX'S OWN PRE-REGISTERED CHECK (6.148 (5)). A row is keyed by the MODULAR slot, and a landmark of the
    # PREVIOUS SFN cycle used to be paired with this cycle's base - printing a delta of -10.24 s. The fix drops a
    # slot's landmarks when its base is refreshed, so (a) no row may carry a delta of about minus one SFN cycle
    # and (b) the header must say how many stale instants were dropped and that none was refused. A leg flown
    # before the fix fails BOTH (that is the negative control this check was written against, p87).
    neg=$(grep -aE '^  *[0-9]+ +[0-9]' "$f" | awk '{for(i=2;i<=6;i++) if ($i+0 < -1000) {n++; break}} END{print n+0}')
    judge "[ul_slot_trace] rows with a delta of minus a cycle" "0 (6.148 (5): the rebase fix)" "$neg" \
          "$([ "$neg" = "0" ] && echo PASS || echo FAIL)"
    ref=$(grep -aoE 'negative deltas refused=[0-9]+' "$f" | tail -1)
    if [ -z "$ref" ]; then
      judge "[ul_slot_trace] refused-negative counter" "the header carries it (fix present)" "<absent>" FAIL
    else
      judge "[ul_slot_trace] refused-negative counter" "0" "${ref#*=}" \
            "$([ "${ref#*=}" = "0" ] && echo PASS || echo FAIL)"
      grep -aoE 'rebased=[0-9]+ landmark' "$f" | tail -1 | sed 's/^/      /'
    fi
  fi

  echo "--- (3) the perturbation self-check (a probe that moves what it measures is void) -------------------"
  tim=$(grep -aE '^\[ul_rx_timing\] calls=' "$f" | tail -1)
  if [ -z "$tim" ]; then
    judge "[ul_rx_timing] readable" "a calls= line" "<none>" FAIL
  else
    lp=$(echo "$tim" | sed 's/.*loop(max=\([0-9]*\)us.*/\1/')
    sl=$(echo "$tim" | sed 's/.*slip(max=\([0-9]*\)us.*/\1/')
    judge "[ul_rx_timing] loop max (host lateness)" "<= 2x p86 (${REF_LOOP_MAX}us)" "${lp}us" \
          "$(awk -v x="$lp" -v r="$REF_LOOP_MAX" 'BEGIN{print (x<=2*r)?"PASS":"FAIL"}')"
    judge "[ul_rx_timing] slip max (timeline drift)" "same order as p86 (${REF_SLIP_MAX}us)" "${sl}us" \
          "$(awk -v x="$sl" -v r="$REF_SLIP_MAX" 'BEGIN{print (x<=4*r)?"PASS":"INFO"}')"
  fi
  cr=$(grep -aE 'host device data crossings:' "$f" | tail -1)
  if [ -z "$cr" ]; then
    judge "crossings (G1 red line)" "a crossings line" "<none>" FAIL
  else
    v=$(echo "$cr" | grep -q '= 0.00 read(s) + 0.00 write(s) per hop' && echo PASS || echo FAIL)
    judge "crossings (G1 red line)" "0.00 + 0.00 per hop" "$(echo "$cr" | grep -oE '= .* per hop')" "$v"
  fi
  ct=$(grep -aE 'contract MET' "$f" | tail -1)
  judge "contract (9 of 9, mode=gpu)" "MET (9 of 9" "$(echo "$ct" | grep -oE 'MET \(9 of 9' || echo '<none>')" \
        "$(echo "$ct" | grep -q 'MET (9 of 9' && echo PASS || echo FAIL)"

  echo "--- (4) V1 next to the p86 baseline ----------------------------------------------------------------"
  v1=$(grep -aE '^\[ul_gpu_pipeline\] samples=' "$f" | tail -1)
  if [ -z "$v1" ]; then
    judge "[ul_gpu_pipeline] readable" "a samples= line" "<none>" FAIL
  else
    m=$(echo "$v1" | sed 's/.*median=\([0-9.]*\)us.*/\1/')
    judge "V1 median vs its criterion" "<= ${V1_CRIT}us" "${m}us" \
          "$(awk -v x="$m" -v c="$V1_CRIT" 'BEGIN{print (x<=c)?"PASS":"FAIL"}')"
    judge "V1 median vs p86 (delivery regression)" "within 15% of ${REF_V1_MED}us" "${m}us" \
          "$(awk -v x="$m" -v r="$REF_V1_MED" 'BEGIN{d=(x>r)?(x-r):(r-x); print (100*d/r<=15)?"PASS":"INFO"}')"
  fi
done

echo
if [ "$n_fail" = "0" ]; then
  echo "probe recheck: no FAIL (see INFO rows - they are readings, not passes)"
else
  echo "probe recheck: $n_fail FAIL  <- read them as 'the instrument did not do what it was registered to do'"
fi
exit "$([ "$n_fail" = "0" ] && echo 0 || echo 1)"
