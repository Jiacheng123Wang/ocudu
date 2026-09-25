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
#       (read against the account line's `series at exit=` once the leg has one; see the 2026-09-25 note below)
#   D1  (Q9, registered in dev doc 6.11 (5)) `input hold (P0-2)` max < 100 ms
#   D2  (Q9) `block lifecycle (P0-7)` `wait max` AND `oldest unclaimed age max` < 100 ms
#   D3  (Q9) `pop_blocking wait (P0-2)` max < 10 ms AND `over 1s=0`
#   D4  (Q9) `radio sample continuity` gaps == 0
#   D5  (Q9, INFO) the two sweep triggers `late=` / `late_time=`, reported and not judged
#       (D1-D4 are the criteria the Q9 fix is confirmed by; a leg flown BEFORE it reads them RED, which is the
#        point - the same leg is what the fix is measured against)
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
  # The ACCOUNT line (2026-09-25): the lane probe asks the pipeline probe for the series count AT EXIT and
  # prints paired / announced / series-at-exit together with a verdict. That is the strongest form of the
  # criterion - all three numbers read at ONE instant - so C2 prefers it and falls back to the pairing line's
  # own two counts for legs flown before it existed.
  account=$(grep -a "paired/phase account (P0-5)" "$LEGF" | tail -1)
  acc_pair=$(printf '%s' "$account" | grep -oE "paired=[0-9]+" | grep -oE "[0-9]+$")
  acc_ann=$(printf '%s' "$account" | grep -oE "announced=[0-9]+" | grep -oE "[0-9]+$")
  acc_exit=$(printf '%s' "$account" | grep -oE "series at exit=[0-9]+" | grep -oE "[0-9]+$")
  acc_verdict=$(printf '%s' "$account" | grep -oE "EXACT MATCH|MISMATCH")
  if [ -n "$account" ] && [ -n "${acc_pair:-}" ]; then
    check "C2 (P0-5) paired == announced == the phase series (all read at exit)" "the account line reads EXACT MATCH" \
          "$([ "${acc_verdict:-}" = "EXACT MATCH" ] && echo PASS || echo FAIL)" \
          "paired=$acc_pair, announced=$acc_ann, series at exit=${acc_exit:-<none>} -> ${acc_verdict:-<no verdict>}; [ul_time_frequency] printed=${n_phase:-<none>}; $account"
    check "C2b (P0-5) the printed [ul_time_frequency] line is an EARLIER snapshot of the same counter" "printed <= series at exit" \
          "$(if [ -z "${n_phase:-}" ] || [ -z "${acc_exit:-}" ]; then echo INFO; elif [ "$n_phase" -le "$acc_exit" ]; then echo PASS; else echo FAIL; fi)" \
          "printed=${n_phase:-<none>}, series at exit=${acc_exit:-<none>} (delta $(( ${acc_exit:-0} - ${n_phase:-0} )) sample(s) finalized between the two reports, which run at different points of the shutdown)"
  elif [ -z "${n_paired:-}" ] || [ -z "${n_announced:-}" ]; then
    check "C2 (P0-5) paired samples == the samples announced to the lane probe" "equal (same line, same instant)" RED \
          "cannot read: paired='${n_paired:-<absent>}' announced='${n_announced:-<absent>}' and no account line - the paired line is: ${paired_line:-<none>}"
  else
    check "C2 (P0-5) paired samples == the samples announced to the lane probe" "equal (same line, same instant)" \
          "$([ "$n_paired" = "$n_announced" ] && echo PASS || echo FAIL)" \
          "paired=$n_paired, announced=$n_announced, [ul_time_frequency] printed=${n_phase:-<none>} (leg predates the account line); $paired_line"
    # The structural half: the printed series is an earlier snapshot of the same counter, so it can only be
    # SMALLER. The other direction would mean the three series shrank - impossible, hence a real defect.
    if [ -z "${n_phase:-}" ]; then
      check "C2b (P0-5) the printed phase series is an earlier snapshot of the same counter" "printed <= announced" \
            INFO "the leg prints no [ul_time_frequency] line (C1 already says whether the segments were on)"
    else
      check "C2b (P0-5) the printed phase series is an earlier snapshot of the same counter" "printed <= announced" \
            "$([ "$n_phase" -le "$n_announced" ] && echo PASS || echo FAIL)" \
            "printed=$n_phase, announced=$n_announced (delta $((n_announced - n_phase)) sample(s) were finalized between the pipeline report and the lane report)"
    fi
  fi
else
  check "C1 a leg that declares the phase segments actually records them" "OCUDU_UL_PHASE_SEGMENTS=1 -> segments present" INFO \
        "not a phases leg (knob unset) - C1 does not apply"
  check "[INFO] C2 (P0-5) is judged only on a leg with the segments on" "OCUDU_UL_PHASE_SEGMENTS=1" INFO \
        "not a phases leg (knob unset): ${paired_line:-<no pairing line>}"
fi
# ---------------------------------------------------------------- Q9: is a block that nobody claims reaped in TIME?
# The criteria are the ones registered in the dev doc 6.11 (5), written BEFORE the confirmation leg existed:
#   D1  input hold (P0-2) max                < 100 ms   (was 30.72 s on the leg that closed Q9)
#   D2  block lifecycle (P0-7) `wait max` AND `oldest unclaimed age max`  < 100 ms  (was 30.72 s / 77.86 s)
#   D3  pop_blocking wait (P0-2) max         < 10 ms AND `over 1s=0`      (was 4.998 s x2)
#   D4  radio sample continuity              gaps == 0                    (was 2 gaps / 153,167,345 samples)
#   D5  the two sweep triggers, reported and NOT judged: `late=` is the total the registry committed itself,
#       `late_time=` the part the TIME deadline claimed. A non-zero `late_time` means the slot window did not
#       cover those blocks (the wrap shape Q9 is about); what that costs depends on the leg, so the number is
#       printed for a person to read rather than compared against a threshold invented here (6.11 (6)).
# Each one reads the leg's OWN report line, and "cannot read" is RED (5.9.97); the thresholds are copied from
# 6.11 (5) verbatim - inventing one inside a gate is how a criterion becomes whatever the last person wanted.
q9_lt() { awk -v v="${1:-}" -v t="$2" 'BEGIN { printf "%d", (v != "" && v+0 < t) ? 1 : 0 }'; }
q9_us() { printf '%s' "$1" | grep -oE "$2=[0-9.]+us" | grep -oE "[0-9.]+" | tail -1; }
q9_ms() { awk -v v="$1" 'BEGIN{printf "%.1f", v/1000}'; }

hold_line=$(grep -a "input hold (P0-2):" "$LEGF" | tail -1)
hold_max=$(q9_us "$hold_line" "max")
if [ -z "$hold_max" ]; then
  check "D1 (Q9) the longest token hold is sub-second" "< 100 ms" RED \
        "cannot read: no 'input hold (P0-2)' line with a max= field - a leg that cannot show the hold cannot show the fix either"
else
  check "D1 (Q9) the longest token hold is sub-second" "< 100 ms" \
        "$([ "$(q9_lt "$hold_max" 100000)" = "1" ] && echo PASS || echo FAIL)" \
        "max hold = $(q9_ms "$hold_max") ms; $hold_line"
fi

life_line=$(grep -a "block lifecycle (P0-7):" "$LEGF" | tail -1)
life_wait=$(printf '%s' "$life_line" | grep -oE "wait max=[0-9.]+us" | grep -oE "[0-9.]+" | tail -1)
life_age=$(printf '%s' "$life_line" | grep -oE "oldest unclaimed age max=[0-9.]+us" | grep -oE "[0-9.]+" | tail -1)
if [ -z "$life_wait" ] || [ -z "$life_age" ]; then
  check "D2 (Q9) an unclaimed block is reaped in ms, not in hyperframes" "wait max < 100 ms AND oldest unclaimed age max < 100 ms" RED \
        "cannot read: lifecycle line='${life_line:-<absent>}'"
else
  check "D2 (Q9) an unclaimed block is reaped in ms, not in hyperframes" "wait max < 100 ms AND oldest unclaimed age max < 100 ms" \
        "$([ "$(q9_lt "$life_wait" 100000)" = "1" ] && [ "$(q9_lt "$life_age" 100000)" = "1" ] && echo PASS || echo FAIL)" \
        "wait_for_a_claim max = $(q9_ms "$life_wait") ms, oldest unclaimed age max = $(q9_ms "$life_age") ms; $life_line"
fi

pop_line=$(grep -a "pop_blocking wait (P0-2):" "$LEGF" | tail -1)
pop_max=$(q9_us "$pop_line" "max")
pop_1s=$(printf '%s' "$pop_line" | grep -oE "over 1s=[0-9]+" | grep -oE "[0-9]+$")
if [ -z "$pop_max" ] || [ -z "${pop_1s:-}" ]; then
  check "D3 (Q9) the receive thread is not parked" "max < 10 ms AND over 1s=0" RED \
        "cannot read: pop_blocking line='${pop_line:-<absent>}'"
else
  check "D3 (Q9) the receive thread is not parked" "max < 10 ms AND over 1s=0" \
        "$([ "$(q9_lt "$pop_max" 10000)" = "1" ] && [ "$pop_1s" = "0" ] && echo PASS || echo FAIL)" \
        "max = $(q9_ms "$pop_max") ms, over 1s = $pop_1s; $pop_line"
fi

cont_line=$(grep -a "radio sample continuity" "$LEGF" | tail -1)
cont_gaps=$(printf '%s' "$cont_line" | grep -oE "[0-9]+ gaps over" | grep -oE "[0-9]+")
if [ -z "${cont_gaps:-}" ]; then
  check "D4 (Q9) the radio lost no sample" "gaps == 0" RED \
        "cannot read: no 'radio sample continuity' line in the leg"
else
  check "D4 (Q9) the radio lost no sample" "gaps == 0" \
        "$([ "$cont_gaps" = "0" ] && echo PASS || echo FAIL)" \
        "$cont_line"
fi

hand_line=$(grep -a "\[metal_stats\] dft handover" "$LEGF" | tail -1)
late_n=$(printf '%s' "$hand_line" | grep -oE " late=[0-9]+" | grep -oE "[0-9]+")
late_t=$(printf '%s' "$hand_line" | grep -oE " late_time=[0-9]+" | grep -oE "[0-9]+")
if [ -z "${late_n:-}" ]; then
  check "[INFO] D5 (Q9) which sweep trigger did the work" "reported, not judged" INFO \
        "no 'dft handover' line with a late= field: ${hand_line:-<absent>}"
else
  check "[INFO] D5 (Q9) which sweep trigger did the work" "reported, not judged" INFO \
        "late=$late_n (blocks the registry committed itself), late_time=${late_t:-<absent>} of them by the TIME deadline$( if [ -z "${late_t:-}" ]; then printf '  (the leg predates the fix: it cannot say which trigger reaped)'; elif [ "${late_t:-0}" != "0" ]; then printf '  <-- the SLOT window could not have caught these (the wrap shape Q9 is about)'; else printf '  (the slot window reaped everything)'; fi )"
fi

# Q9-A / Q9-B (dev doc 6.13): the readings that say WHERE a stall lives. Both INFO by construction - the
# second is read against the first, and the pair is what picks the next branch (6.13 (4)), not a threshold.
#   D6  `registry commit->completion=` next to `deposit->completion`: equal maxima = the seconds came AFTER a
#       commit the registry had already issued; ms against seconds = they came BEFORE it (the holder is the
#       claim/hold side, not the commit).
#   D7  `dry-pool reaps=N recovering M block(s)`: events > 0 with blocks = 0 says the pool was held by a
#       CLAIMED block (the sweep must not touch those) rather than by an unclaimed one - the difference
#       between "nobody asked the registry" and "the registry had nothing it was allowed to reap".
life_commit=$(printf '%s' "$life_line" | grep -oE "registry commit->completion=[0-9]+ max=[0-9.]+us" | head -1)
life_commit_max=$(printf '%s' "$life_commit" | grep -oE "max=[0-9.]+us" | grep -oE "[0-9.]+")
if [ -z "${life_commit:-}" ]; then
  check "[INFO] D6 (Q9-B) which half of the completion wait it was" "reported, not judged" INFO \
        "no 'registry commit->completion=' field: ${life_line:-<absent>} - a leg flown before 6.13 cannot say"
else
  check "[INFO] D6 (Q9-B) which half of the completion wait it was" "reported, not judged" INFO \
        "$life_commit; deposit->completion max=${life_wait:-<none>}us -> $(awk -v a="$life_commit_max" -v b="${life_wait:-0}" 'BEGIN{ if (b+0 <= 0) printf "no completion to compare"; else if (a+0 >= 0.5*b) printf "the seconds came AFTER the registry committed it (%.0f%% of the wait)", 100*a/b; else printf "the seconds came BEFORE the commit (%.0f%% after it) - the holder is the claim/hold side", 100*a/b }')"
fi
reaps_events=$(printf '%s' "$life_line" | grep -oE "dry-pool reaps=[0-9]+" | grep -oE "[0-9]+$")
reaps_blocks=$(printf '%s' "$life_line" | grep -oE "recovering [0-9]+ block" | grep -oE "[0-9]+")
if [ -z "${reaps_events:-}" ]; then
  check "[INFO] D7 (Q9-A) did a DRY pool drive the sweep" "reported, not judged" INFO \
        "no 'dry-pool reaps=' field: ${life_line:-<absent>} - a leg flown before 6.13 cannot say"
else
  check "[INFO] D7 (Q9-A) did a DRY pool drive the sweep" "reported, not judged" INFO \
        "events=$reaps_events recovering ${reaps_blocks:-0} block(s)$( [ "${reaps_events:-0}" != "0" ] && [ "${reaps_blocks:-0}" = "0" ] && printf '  <-- the pool was held by CLAIMED block(s): the sweep is not allowed to touch those, so the holder is the claim/hold side' || true )"
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
