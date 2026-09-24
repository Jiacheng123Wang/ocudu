#!/usr/bin/env bash
# P0 gate (doc_chinese/phy_latency/gpu_phy_latency_optimization_design_and_implementation.md, 6.1-6.3):
# read the P0 instruments out of a leg, and
# judge them against the criteria that were written down BEFORE the readings existed.
#
# It READS LOG FILES ONLY - it never starts a gNB, a replay or anything that touches the GPU, so it is
# safe to run while the operator is flying a leg.
#
# usage:
#   bash p0_gate.sh <leg-label|path>                      # P0-6 (always) + P0-1 (if the leg is a split arm)
#   bash p0_gate.sh <split-leg> --vs=<factory-leg>        # + the "+-10% of the factory arm's merged_hop" criterion
#
# Criteria (registered in the latency dev doc 6.1 (P0-6) and 6.2 (P0-1); C2 in 6.3):
#   A1  the two [ul_lane_exec] lines are present, and the value they report equals the executor's own
#       max_concurrency (self-consistency: the resolved value and the shape must agree)
#   A2  the shape follows the rule: <= 1 must read STRAND, > 1 must read "task fork limiter"
#   B1  a split arm's `busy split` gains a `dft=` token whose cbs/lane is 1.00 (the front end's buffer is
#       its own submission), and a factory arm's does NOT gain one
#   B2  (only with --vs) dft + (the rest) is within +-10% of the factory arm's merged_hop
#   C1  a leg that declares OCUDU_UL_PHASE_SEGMENTS=1 actually records the phase segments
#   C2  (P0-5, registered in phy_latency/session_handoff_2026-09-24-1.md 6.0 (1)) the samples the lane probe
#       could pair with a lane EQUAL the samples the pipeline probe announced to it - the two probes describing
#       one population. Read from the SAME line (paired `samples=` vs `phase_samples=`); the `[ul_time_frequency]`
#       line is a snapshot taken earlier in the shutdown, and C2b reports - and bounds by SIGN - that delta
#   C2b the printed `[ul_time_frequency]` count is an earlier snapshot of the same counter: printed <= announced
# "Cannot read" is RED, never absent - the lesson of 5.9.97.
set -u

LEG=""
VS=""
for a in "$@"; do
  case "$a" in
    --vs=*) VS=${a#*=} ;;
    *)      LEG=$a ;;
  esac
done
[ -n "$LEG" ] || { echo "usage: bash p0_gate.sh <leg-label|path> [--vs=<factory-leg>]" >&2; exit 2; }

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
LOGDIR=$ROOT/doc_chinese/phy_pipeline_gpu/wip/logs

resolve() {
  if [ -f "$1" ]; then printf '%s' "$1"; return; fi
  local hit
  hit=$(ls -1t "$LOGDIR"/gnb_*"$1"*.log.stderr 2>/dev/null | head -1)
  [ -n "$hit" ] || { echo "no leg matched '$1' in $LOGDIR" >&2; exit 2; }
  printf '%s' "$hit"
}
LEGF=$(resolve "$LEG")

rows=()
check() { rows+=("$1|$2|$3|$4"); }   # name|expected|verdict|detail

# ---------------------------------------------------------------- P0-6: the lane's effective concurrency
conc=$(grep -aoE "\[ul_lane_exec\] PUSCH/SRS concurrency = [0-9]+" "$LEGF" | tail -1 | grep -oE "[0-9]+$")
shape=$(grep -aoE "\[ul_lane_exec\] PUSCH lane executor:.*" "$LEGF" | tail -1)
exec_conc=$(printf '%s' "$shape" | grep -oE "pusch_executor\.max_concurrency=[0-9]+" | grep -oE "[0-9]+$")
line1=$(grep -aE "\[ul_lane_exec\] PUSCH/SRS concurrency" "$LEGF" | tail -1)

if [ -z "$conc" ] || [ -z "$exec_conc" ]; then
  check "A1 the two [ul_lane_exec] lines are present and agree" "value == pusch_executor.max_concurrency" RED \
        "cannot read: concurrency='${conc:-<none>}' executor='${exec_conc:-<none>}'"
else
  check "A1 the two [ul_lane_exec] lines are present and agree" "value == pusch_executor.max_concurrency" \
        "$([ "$conc" = "$exec_conc" ] && echo PASS || echo FAIL)" \
        "concurrency=$conc, pusch_executor.max_concurrency=$exec_conc"
fi

if [ -z "$shape" ]; then
  check "A2 the shape follows the rule (<=1 strand, >1 fork)" "STRAND iff value <= 1" RED "no executor line"
else
  if [ "${exec_conc:-1}" -le 1 ]; then want="STRAND"; else want="fork limiter"; fi
  check "A2 the shape follows the rule (<=1 strand, >1 fork)" "STRAND iff value <= 1" \
        "$(printf '%s' "$shape" | grep -q "$want" && echo PASS || echo FAIL)" \
        "expected '$want' for value ${exec_conc:-?}: $shape"
fi

# ---------------------------------------------------------------- P0-1: the split arm's dft group
split=$(grep -a "busy split" "$LEGF" | tail -1)
dft=$(printf '%s' "$split" | grep -oE "dft=[0-9.]+us/lane \([0-9]+% of busy, cbs/lane=[0-9.]+\)")
# NOT line-anchored: run_leg.sh's provenance glues its 'knob :' lines to the previous line (measured
# on s82 and s86), so a ^knob grep reads nothing and this criterion silently judges the wrong arm - which
# is exactly what happened on s86 (a real split arm read as 'knob off').
knob=$(grep -aoE "OCUDU_LANE_DIAG_SPLIT=[0-9]+" "$LEGF" | tail -1 | sed 's/.*=//')
if [ -z "$split" ]; then
  check "B1 the split arm gains a dft= token (factory arm does not)" "dft present iff the knob is on" RED "no 'busy split' line"
elif [ -z "${knob:-}" ] && [ -n "$dft" ]; then
  check "B1 the split arm gains a dft= token (factory arm does not)" "dft present iff the knob is on" RED \
        "dft= is present but the leg records no OCUDU_LANE_DIAG_SPLIT at all: either the DEFAULT changed (a real defect) or run_leg.sh failed to record the knob - read how the leg was launched. Read: $dft"
elif [ "${knob:-0}" != "0" ] && [ -n "${knob:-}" ]; then
  check "B1 the split arm gains a dft= token (factory arm does not)" "dft present iff the knob is on" \
        "$([ -n "$dft" ] && echo PASS || echo FAIL)" \
        "OCUDU_LANE_DIAG_SPLIT=$knob -> ${dft:-<absent>}"
else
  check "B1 the split arm gains a dft= token (factory arm does not)" "dft present iff the knob is on" \
        "$([ -z "$dft" ] && echo PASS || echo FAIL)" \
        "knob off -> ${dft:-<absent, as expected>}"
fi

# B2: the groups must add up to the FACTORY arm's merged_hop. Read only when --vs is given.
sum_group=""
if [ -n "$split" ]; then
  sum_group=$(printf '%s' "$split" | grep -oE "[a-z_]+=[0-9.]+us/lane" | sed 's/.*=//;s/us\/lane//' | awk '{s+=$1} END {printf "%.1f", s+0}')
fi
if [ -n "$VS" ]; then
  VSF=$(resolve "$VS")
  vs_split=$(grep -a "busy split" "$VSF" | tail -1)
  vs_merged=$(printf '%s' "$vs_split" | grep -oE "merged_hop=[0-9.]+us/lane" | grep -oE "[0-9.]+")
  if [ -z "$sum_group" ] || [ -z "$vs_merged" ]; then
    check "B2 the groups sum to the factory arm's merged_hop (+-10%)" "factory merged_hop * 0.9 .. * 1.1" RED \
          "cannot read: split-sum='${sum_group:-<none>}' factory merged_hop='${vs_merged:-<none>}'"
  else
    ok=$(awk -v s="$sum_group" -v m="$vs_merged" 'BEGIN { printf "%d", (s >= 0.9*m && s <= 1.1*m) ? 1 : 0 }')
    check "B2 the groups sum to the factory arm's merged_hop (+-10%)" "factory merged_hop * 0.9 .. * 1.1" \
          "$([ "$ok" = "1" ] && echo PASS || echo FAIL)" \
          "split groups sum = ${sum_group}us/lane vs factory merged_hop = ${vs_merged}us/lane (ratio $(awk -v s="$sum_group" -v m="$vs_merged" 'BEGIN{printf "%.3f", s/m}'))"
  fi
else
  check "[INFO] B2 needs a factory arm to compare against" "pass --vs=<factory-leg>" INFO \
        "busy-split total = ${sum_group:-<unreadable>}us/lane (no --vs given)"
fi

# ---------------------------------------------------------------- P0-5: are the two probes paired?
# Judged ONLY on a leg that declares the knob, exactly like B1: a leg that never asked for the phase
# segments cannot be failed for not having them (that is how a criterion gets bound to the wrong arm).
knob_ph=$(grep -aoE "OCUDU_UL_PHASE_SEGMENTS=[0-9]+" "$LEGF" | tail -1 | sed 's/.*=//')
n_phase=$(grep -aoE "\[ul_time_frequency\] samples=[0-9]+" "$LEGF" | tail -1 | grep -oE "[0-9]+$")
n_lane=$(grep -aoE "\[ul_gpu_lane\] residency samples=[0-9]+" "$LEGF" | tail -1 | grep -oE "[0-9]+$")
# The pairing report (P0-5). Its own sample count is the number of phase samples the lane probe could pair with
# the lane that produced them, and `phase_samples` is what the pipeline probe announced to it - the SAME series
# the [ul_time_frequency] line above counts, printed side by side so a reader can see the join's own accounting
# instead of inferring it from two lines that were recorded by different probes.
paired_line=$(grep -a "\[ul_gpu_lane\] paired with the phase segments" "$LEGF" | tail -1)
n_paired=$(printf '%s' "$paired_line" | grep -oE "P0-5\): samples=[0-9]+" | grep -oE "[0-9]+$")
n_announced=$(printf '%s' "$paired_line" | grep -oE "phase_samples=[0-9]+" | grep -oE "[0-9]+$")
if [ -n "${knob_ph:-}" ] && [ "${knob_ph:-0}" != "0" ]; then
  check "C1 a leg that declares the phase segments actually records them" "OCUDU_UL_PHASE_SEGMENTS=1 -> segments present" \
        "$([ -n "${n_phase:-}" ] && [ "${n_phase:-0}" -gt 0 ] && echo PASS || echo FAIL)" \
        "knob=$knob_ph, phase-segment samples=${n_phase:-<none>}"
  # C2: the criterion P0-5 was commissioned with (phy_latency/session_handoff_2026-09-24-1.md 6.0 (1)): the
  # paired population must BE the phase-segment population - a subset would mean the join drops samples, and a
  # superset that it pairs lanes with samples that do not exist. "Cannot read" is RED, never absent (5.9.97):
  # a leg that asked for the segments and printed no pairing line is a failure of the instrument, not a pass.
  # The counts compared are the ones the SAME line prints: `samples=` (what the lane probe could pair) against
  # `phase_samples=` (what the pipeline probe announced to it). They are read at the same instant, which is what
  # makes the criterion exact.
  #
  # NOT against the `[ul_time_frequency] samples=` line, and that is a reading rule rather than a relaxation:
  # gnb.cpp calls ul_pipeline_probe::report() EARLY in the shutdown while the lane probe reports at exit, and the
  # observer keeps counting until then - so the series line is a SNAPSHOT that can be short by the samples
  # finalized in between (measured on `p05-pair`: 73528 printed against 73529 paired/announced, i.e. ONE sample,
  # 0.0014%). Comparing against it read a false C2 FAIL on a leg whose pairing was exact. The delta is REPORTED
  # below and checked for sign only (the series cannot shrink), so nothing is hidden and no threshold is invented.
  if [ -z "${n_paired:-}" ] || [ -z "${n_announced:-}" ]; then
    check "C2 (P0-5) paired samples == the samples announced to the lane probe" "equal (same line, same instant)" RED \
          "cannot read: paired='${n_paired:-<absent>}' announced='${n_announced:-<absent>}' - the paired line is: ${paired_line:-<none>}"
  else
    check "C2 (P0-5) paired samples == the samples announced to the lane probe" "equal (same line, same instant)" \
          "$([ "$n_paired" = "$n_announced" ] && echo PASS || echo FAIL)" \
          "paired=$n_paired, announced=$n_announced, [ul_time_frequency] printed=${n_phase:-<none>}; $paired_line"
    # The structural half: the printed series is an earlier snapshot of the same counter, so it can only be
    # SMALLER. The other direction would mean the three series shrank - impossible, hence a real defect.
    if [ -z "${n_phase:-}" ]; then
      check "C2b (P0-5) the printed phase series is an earlier snapshot of the same counter" "printed <= announced" \
            INFO "the leg prints no [ul_time_frequency] line (C1 already says whether the segments were on)"
    else
      check "C2b (P0-5) the printed phase series is an earlier snapshot of the same counter" "printed <= announced" \
            "$([ "$n_phase" -le "$n_announced" ] && echo PASS || echo FAIL)" \
            "printed=$n_phase, announced=$n_announced (delta $((n_announced - n_phase)) sample(s) were finalized between the pipeline report and the lane report; the report runs at the start of the shutdown, the lane report at exit)"
    fi
  fi
else
  check "C1 a leg that declares the phase segments actually records them" "OCUDU_UL_PHASE_SEGMENTS=1 -> segments present" INFO \
        "not a phases leg (knob unset) - C1 does not apply"
  check "[INFO] C2 (P0-5) is judged only on a leg with the segments on" "OCUDU_UL_PHASE_SEGMENTS=1" INFO \
        "not a phases leg (knob unset): ${paired_line:-<no pairing line>}"
fi
# The ratio the pairing was commissioned to recompute (6.0 (2)): printed, never judged - the criterion says
# "state whether it is still ~95%", not "fail below it", and inventing a threshold inside a gate is how a
# criterion becomes whatever the last person wanted (5.9.101).
ratios_line=$(grep -a "\[ul_gpu_lane\] paired ratios (P0-5)" "$LEGF" | tail -1)
reading_line=$(grep -a "\[ul_gpu_lane\] paired reading (P0-5)" "$LEGF" | tail -1)
check "[INFO] P0-5: the paired ratios and whether the reading survives" "reported, not judged" INFO \
      "${ratios_line:-<no paired ratios line>}  ||  ${reading_line:-<no paired reading line>}"
check "[INFO] the two probes' populations (not judged - no threshold registered)" \
      "phase samples vs lane residency samples" INFO \
      "phase-segment samples=${n_phase:-<none>}, lane residency samples=${n_lane:-<none>}$([ -n "${n_lane:-}" ] && [ "${n_lane:-0}" != "0" ] && [ -n "${n_phase:-}" ] && awk -v a="$n_phase" -v b="$n_lane" 'BEGIN{printf "  (unpaired ratio %.3f)", a/b}')"

# ---------------------------------------------------------------- INFO: what else this leg carries
phase=$(grep -aoE "\[ul_time_frequency\][^\"]{0,40}" "$LEGF" | tail -1)
resid=$(grep -aoE "\[ul_gpu_lane\] residency samples=[0-9]+" "$LEGF" | tail -1)
check "[INFO] the leg's other P0 readings" "reported, not judged" INFO \
      "busy split: ${split:-<none>}  ||  ${resid:-<no residency line>}  ||  ${phase:-<no phase segments>}"

echo "P0 gate: $(basename "$LEGF")${VS:+   (factory arm: $VS)}"
echo
red=0
for r in "${rows[@]}"; do
  IFS='|' read -r name exp verdict detail <<<"$r"
  printf '  [%-16s] %s\n' "$verdict" "$name"
  printf '                     expected: %s\n' "$exp"
  printf '                     read    : %s\n' "$detail"
  [ "$verdict" = "PASS" ] || [ "$verdict" = "INFO" ] || red=$((red+1))
done
echo
echo "  $(( ${#rows[@]} - red )) of ${#rows[@]} criteria pass$([ "$red" = 0 ] && echo '' || echo "  --  $red to explain")"
echo "  Read P0-6's lines verbatim with:  grep -a ul_lane_exec $LEGF"
