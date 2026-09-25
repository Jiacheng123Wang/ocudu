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
#   D6/D7 (Q9-A/-B, INFO) which half of the completion wait it was, and whether a DRY pool drove the sweep
#   D8  (Q9-C, INFO) own / newest / cross_lane: how often the burst's stage-fence wait would have named
#       another lane's estimator generation under the old rule (dev doc 6.14)
#   D9  (Q9-D, INFO) signaller-first / signaller-after and the longest such wait: whether a device-side fence
#       wait named a signal that had not been handed out yet, i.e. which fence (if any) held a queue (6.16)
#   D10 (Q9-E, INFO) the host's own lag between the GPU finishing a block and its completion handler running:
#       the input tokens are released by that handler, so this is what separates "the queue held it" from
#       "the host was late to look" (6.17)
#   D11 (Q9-F, INFO) was a WAITER's command buffer COMMITTED before the command buffer carrying the signal it
#       waits for, and on the same queue: Q9-D compares against the moment a generation was handed out, which
#       is not when it is submitted - the registry's sweep commits a claimed block from another thread (6.19)
#   D12 (Q9-F2/F3, INFO) what the device was doing while a waiter waited: the holes in the union of the probed
#       GPU windows (probe: OCUDU_METAL_GPU_TIME=1) and the front-end blocks that were not final when their own
#       slot's group closed - the population no earlier instrument reported at all (6.19)
#   D15 (6.26, INFO) fix B: how many blocks a DRY pool dropped (instead of parking the receive thread and
#       letting the radio's ring overflow). The criterion it serves is D4 (gaps)
#   D16 (6.30, INFO) the batched front end: how many front-end dispatches carried more than one transform
#       (`OCUDU_DFT_BATCH_SYMBOLS=N`) and the knob the run asked for - `batch_max=1` is the A/B control
#   D14 (6.24, INFO) the on-demand P0 dump: printed by the receive thread when it has been parked on a dry pool
#       for more than 20 ms, i.e. when a stall is HAPPENING - the readings below it are a snapshot from inside it
#   D13 (6.20/6.21, INFO) the commit handshake: how often a consumer was handed a generation whose carrier had
#       not been committed yet (each of those was a 5.00 s queue hold before the handshake, 6.20) and how often
#       that confirmation never came (the consumer is then ordered on the HOST; must be 0)
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
# The DEPOSIT -> completion maximum (not the claim wait): D6 compares the registry's own commit -> completion
# against THIS, which is the only pair that answers "was the wait before or after the commit".
life_prod_max=$(printf '%s' "$life_line" | grep -oE "deposit->completion max=[0-9.]+us" | grep -oE "[0-9.]+" | tail -1)
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
        "$life_commit; deposit->completion max=${life_prod_max:-<none>}us -> $(awk -v a="$life_commit_max" -v b="${life_prod_max:-0}" 'BEGIN{ if (b+0 <= 0) printf "no completion to compare"; else if (a+0 >= 0.5*b) printf "the seconds came AFTER the registry committed it (%.0f%% of the wait)", 100*a/b; else printf "the seconds came BEFORE the commit (%.0f%% after it) - the holder is the claim/hold side", 100*a/b }')"
fi
# D8 (Q9-C, dev doc 6.14): which generation the lane burst's stage fence named. `cross_lane` is how often the
# global newest differed from the hop's own at that moment, i.e. how often the OLD rule would have waited for
# another lane's estimator - an upper bound on the cross-lane pinch, not a count of deadlocks.
fence_line=$(grep -a "\[metal_stats\] lane fence" "$LEGF" | tail -1)
own_n=$(printf '%s' "$fence_line" | grep -oE "own=[0-9]+" | grep -oE "[0-9]+$")
newest_n=$(printf '%s' "$fence_line" | grep -oE "newest=[0-9]+" | grep -oE "[0-9]+$")
cross_n=$(printf '%s' "$fence_line" | grep -oE "cross_lane=[0-9]+" | grep -oE "[0-9]+$")
if [ -z "${own_n:-}" ]; then
  check "[INFO] D8 (Q9-C) which generation the burst's stage fence named" "reported, not judged" INFO \
        "no own=/newest=/cross_lane= fields: ${fence_line:-<absent>} - a leg flown before 6.14 cannot say"
else
  check "[INFO] D8 (Q9-C) which generation the burst's stage fence named" "reported, not judged" INFO \
        "own=$own_n (waits that named THIS hop's own estimator generation), newest=${newest_n:-?} (fallback to the global newest), cross_lane=${cross_n:-?}$( [ -n "${cross_n:-}" ] && [ "${cross_n:-0}" != "0" ] && printf '  <-- the OLD rule would have waited for another lane %s time(s): the cross-lane pinch of 6.14 happened on this leg' "$cross_n" || printf '  (no cross-lane pinch was seen)' )"
fi
# D9 (Q9-D, dev doc 6.16): whether any device-side fence wait named a signaller that had NOT been handed out
# yet - the ordering that can hold a serial queue - and how long such a wait then lasted. This is the reading
# that decides whether a stall sits in a fence at all, and which one.
fence_line2=$(grep -a "fence order (Q9-D)" "$LEGF" | tail -1)
fo_before=$(printf '%s' "$fence_line2" | grep -oE "signaller-first=[0-9]+" | grep -oE "[0-9]+$")
fo_after=$(printf '%s' "$fence_line2" | grep -oE "signaller-after=[0-9]+" | grep -oE "[0-9]+$")
fo_maxms=$(printf '%s' "$fence_line2" | grep -oE "max=[0-9.]+ms" | grep -oE "[0-9.]+")
if [ -z "${fo_after:-}" ]; then
  check "[INFO] D9 (Q9-D) did a fence wait name a signaller that was not out yet" "reported, not judged" INFO \
        "no 'fence order (Q9-D)' line: ${fence_line2:-<absent>} - a leg flown before 6.16 cannot say"
else
  check "[INFO] D9 (Q9-D) did a fence wait name a signaller that was not out yet" "reported, not judged" INFO \
        "signaller-first=$fo_before, signaller-after=$fo_after, longest such wait=${fo_maxms:-?}ms$([ "${fo_after:-0}" != "0" ] && printf '  <-- a fence wait named a signal that was not handed out yet: if this leg stalled, THAT is the fence to read' || printf '  (every wait named a signaller that was already out)' )  ||  $fence_line2"
fi
# D10 (Q9-E, dev doc 6.17): how long after the GPU FINISHED a block its completion handler ran. The input
# tokens are released by that handler, so a handler dispatched late holds the pool exactly as a buffer that
# runs late does - and `deposit->completion` cannot tell the two apart. A large value here says the GPU was
# done and the HOST was late; a small one, next to a large `commit->completion`, says the buffer itself waited.
lag_max=$(printf '%s' "$life_line" | grep -oE "handler lag=[0-9]+ max=[0-9.]+us" | grep -oE "max=[0-9.]+us" | grep -oE "[0-9.]+")
lag_slot=$(printf '%s' "$life_line" | grep -oE "at slot=[0-9]+ \(Q9-E" | grep -oE "[0-9]+")
if [ -z "${lag_max:-}" ]; then
  check "[INFO] D10 (Q9-E) GPU finished -> the handler ran" "reported, not judged" INFO \
        "no 'handler lag=' field: ${life_line:-<absent>} - a leg flown before 6.17 cannot say"
else
  lag_note=$(awk -v v="$lag_max" -v d="${life_prod_max:-0}" 'BEGIN{ if (d+0 > 0 && v+0 >= 0.5*d) printf "  <-- the seconds were the HOST side after the GPU was done, not a queue wait"; else printf "  (the host was prompt: a late completion is the buffer own)" }')
  check "[INFO] D10 (Q9-E) GPU finished -> the handler ran" "reported, not judged" INFO \
        "max handler lag = $(awk -v v="$lag_max" 'BEGIN{printf "%.1f", v/1000}') ms at slot=${lag_slot:-?}$lag_note  ||  $life_line"
fi
# D11 (Q9-F, dev doc 6.19): Q9-D's blind spot, measured on the COMMIT order instead of the moment a generation
# was handed out. A signal rides a command buffer, and that buffer can be committed by ANOTHER thread after the
# waiter's commit (the registry's sweep commits a claimed hand-over block after dropping its lock) - on a queue
# that only orders starts, a waiter ahead of its signaller is a queue that cannot get past the wait.
fence_line3=$(grep -a "commit order (Q9-F)" "$LEGF" | tail -1)
co_waits=$(printf '%s' "$fence_line3" | grep -oE "waits=[0-9]+" | grep -oE "[0-9]+$")
co_first=$(printf '%s' "$fence_line3" | grep -oE "waiter-committed-first=[0-9]+" | grep -oE "[0-9]+$")
co_same=$(printf '%s' "$fence_line3" | grep -oE "same-queue=[0-9]+" | grep -oE "[0-9]+$")
co_cross=$(printf '%s' "$fence_line3" | grep -oE "cross-queue=[0-9]+" | grep -oE "[0-9]+$")
co_maxms=$(printf '%s' "$fence_line3" | grep -oE "max=[0-9.]+ms" | grep -oE "[0-9.]+")
co_kind=$(printf '%s' "$fence_line3" | grep -oE "worst kind=[a-z]+" | cut -d= -f2)
co_slot=$(printf '%s' "$fence_line3" | grep -oE "slot=[0-9]+" | grep -oE "[0-9]+$")
if [ -z "${co_first:-}" ]; then
  check "[INFO] D11 (Q9-F) was a WAITER committed before its signaller" "reported, not judged" INFO \
        "no 'commit order (Q9-F)' line: ${fence_line3:-<absent>} - a leg flown before 6.19 cannot say"
else
  check "[INFO] D11 (Q9-F) was a WAITER committed before its signaller" "reported, not judged" INFO \
        "waits=$co_waits, waiter-committed-first=$co_first (same-queue=${co_same:-?} = the shape that cannot resolve itself, cross-queue=${co_cross:-?}), longest=${co_maxms:-?}ms worst kind=${co_kind:-?} slot=${co_slot:-?}$( [ "${co_first:-0}" != "0" ] && printf '  <-- the Q9-G ordering IS on this leg: if its longest wait is the stall, the fix is the commit handshake' || printf '  (every waiter was committed after a signaller at or above its value: the queue-order hypothesis is NOT confirmed here)' )"
fi
# D12 (Q9-F2/Q9-F3, dev doc 6.19): what the DEVICE was doing while a waiter waited. The queue-occupancy union
# (probe on: OCUDU_METAL_GPU_TIME=1) and the front-end blocks that were not final when their own slot's group
# closed - with the hand-over armed those are the rule, and no earlier instrument reported them at all.
# D13 (dev doc 6.20/6.21): the commit handshake. `waits` > 0 says a consumer was handed a generation whose
# carrier had not been committed yet - the Q9-G window - and that the handshake closed it; `timeouts` must stay
# 0 (a non-zero value means a consumer had to be ordered on the HOST because the commit never came).
shake_line=$(grep -a "handshake=waits:" "$LEGF" | tail -1)
shake_waits=$(printf '%s' "$shake_line" | grep -oE "handshake=waits:[0-9]+" | grep -oE "[0-9]+$")
shake_timeouts=$(printf '%s' "$shake_line" | grep -oE "timeouts:[0-9]+" | grep -oE "[0-9]+$")
shake_max=$(printf '%s' "$shake_line" | grep -oE "max:[0-9]+us" | grep -oE "[0-9]+")
if [ -z "${shake_waits:-}" ]; then
  check "[INFO] D13 (6.20) did a consumer meet an uncommitted carrier" "reported, not judged" INFO \
        "no 'handshake=waits:' field: ${shake_line:-<absent>} - a leg flown before 6.21 cannot say"
else
  check "[INFO] D13 (6.20) did a consumer meet an uncommitted carrier" "reported, not judged" INFO \
        "handshake waits=$shake_waits timeouts=${shake_timeouts:-?} max=${shake_max:-?}us$([ "${shake_waits:-0}" != "0" ] && printf '  <-- the Q9-G window IS reached on air: without the handshake each of those was a 5.00 s queue hold (6.20)' || printf '  (no consumer met an uncommitted carrier)' )$([ "${shake_timeouts:-0}" != "0" ] && printf '  <-- a consumer had to be ordered on the HOST: the carrier was never committed (must be 0)' || true)"
fi
# D14 (dev doc 6.24): the ON-DEMAND dump. The receive thread prints every P0 reading when it has been parked on
# a dry pool for more than 20 ms - which is the stall itself (it stops consuming the radio, so no slot
# indication is produced and the whole slot loop stops with it). A leg with a dump here STALLED; a healthy leg
# never prints one (measured park: 22 us on n1, 486 us at 12.9 Mbit/s on n78). The readings below it are a
# SNAPSHOT from inside the stall - the same lines the exit report carries.
# D15 (fix B, dev doc 6.26): the blocks a DRY pool DROPPED instead of parking the radio. `dropped > 0` says the
# pipeline went dry on this leg (the same event `starved_events` counts) and that the drop is what kept it from
# becoming lost samples (D4). A leg with `dropped = 0` never had to choose.
drop_field=$(grep -a "ul_rx_pool\] taken=" "$LEGF" | tail -1)
drop_n=$(printf '%s' "$drop_field" | grep -oE "dropped=[0-9]+" | grep -oE "[0-9]+$")
drop_park=$(printf '%s' "$drop_field" | grep -oE "drop_park_max=[0-9]+us" | grep -oE "[0-9]+")
if [ -z "${drop_n:-}" ]; then
  check "[INFO] D15 (6.26) did a dry pool drop blocks instead of parking the radio" "reported, not judged" INFO \
        "no 'dropped=' field: ${drop_field:-<absent>} - a leg flown before 6.26 cannot say"
elif [ "${drop_n}" = "0" ]; then
  check "[INFO] D15 (6.26) did a dry pool drop blocks instead of parking the radio" "reported, not judged" INFO \
        "dropped=0: the pool never stayed dry past the 1 ms budget - no block had to be sacrificed"
else
  check "[INFO] D15 (6.26) did a dry pool drop blocks instead of parking the radio" "reported, not judged" INFO \
        "dropped=$drop_n block(s), longest park before a drop=${drop_park:-?}us  <-- the pool DID go dry; these blocks' samples were discarded so the radio kept streaming (with OCUDU_UL_RX_POOL_DROP=0 the same leg loses them as a `Receive stream discontinuity` instead)"
fi
dump_line=$(grep -a "p0 dump #" "$LEGF" | tail -1)
dump_n=$(grep -ac "p0 dump #" "$LEGF")
if [ "${dump_n:-0}" = "0" ]; then
  check "[INFO] D14 (6.24) did the receive thread park long enough to dump" "reported, not judged" INFO \
        "no 'p0 dump' line: the pool never parked the receive thread for 20 ms - no stall on this leg"
else
  check "[INFO] D14 (6.24) did the receive thread park long enough to dump" "reported, not judged" INFO \
        "$dump_n snapshot(s), last: ${dump_line:-?}  <-- THIS LEG STALLED: read the lines below the dump as readings taken DURING the stall"
fi
# D16 (batched front end, dev doc 6.30): the front-end dispatches that carried MORE THAN ONE transform, and the
# knob the run asked for. `batch_max=1` is the A/B control arm (one dispatch per symbol, the historical shape);
# `batch_max>1` with `batched=0/0` is a FINDING - the deferral never happened (no slot took the radio-input
# path inside an open block), not a silent no-op. The reading is INFO: what the batching is worth on air is V1's
# business, and this line only says whether the mechanism ran.
batch_line=$(grep -a "\[metal_stats\] dft commits=" "$LEGF" | tail -1)
batch_pair=$(printf '%s' "$batch_line" | grep -oE "batched=[0-9]+/[0-9]+" | head -1)
batch_max=$(printf '%s' "$batch_line" | grep -oE "batch_max=[0-9]+" | grep -oE "[0-9]+$")
batch_src=$(printf '%s' "$batch_line" | grep -oE "batch_src=[a-z]+" | head -1)
slot_syms=$(printf '%s' "$batch_line" | grep -oE "slot_symbols=[0-9]+" | grep -oE "[0-9]+$")
# 6.33: with the knob at AUTO the cap IS the cell's own symbol count (14 normal CP, 12 extended), told by the
# demodulator through set_slot_symbols(); AUTO with slot_symbols=0 means NOBODY told the front end how big a
# slot is, which is a wiring finding (the mechanism is defined on one slot, so it declines to guess).
if [ -z "${batch_pair:-}" ]; then
  check "[INFO] D16 (6.30) did the front end batch a slot's transforms into one dispatch" "reported, not judged" INFO \
        "no 'batched=' field: ${batch_line:-<absent>} - a leg flown before 6.30 cannot say"
elif [ "${batch_src:-}" = "batch_src=auto" ] && [ -z "${slot_syms:-}" ]; then
  check "[INFO] D16 (6.30) did the front end batch a slot's transforms into one dispatch" "reported, not judged" INFO \
        "${batch_pair} batch_max=${batch_max}: 'batch_src'/'slot_symbols' absent - the build is older than the AUTO default (dev doc 6.33); read batch_max as the knob's own value"
elif [ "${batch_src:-}" = "batch_src=auto" ] && [ "${slot_syms:-0}" = "0" ]; then
  check "[INFO] D16 (6.30) did the front end batch a slot's transforms into one dispatch" "reported, not judged" INFO \
        "${batch_pair} batch_max=${batch_max} batch_src=auto slot_symbols=0: NOBODY TOLD THE FRONT END how many symbols a slot carries, so it did not batch - the demodulator's set_slot_symbols() did not reach the engine (wiring)"
elif [ "${batch_pair}" = "batched=0/0" ] && [ "${batch_max:-1}" != "1" ]; then
  check "[INFO] D16 (6.30) did the front end batch a slot's transforms into one dispatch" "reported, not judged" INFO \
        "${batch_pair} batch_max=${batch_max} ${batch_src:-} slot_symbols=${slot_syms:-?}: THE DEFERRAL NEVER HAPPENED - no radio-input transform was submitted inside an open block, so this leg is not an A/B of the batched front end"
elif [ "${batch_max:-1}" = "1" ]; then
  check "[INFO] D16 (6.30) did the front end batch a slot's transforms into one dispatch" "reported, not judged" INFO \
        "${batch_pair} batch_max=1 ${batch_src:-}: this leg ran the PER-SYMBOL shape (the A/B control arm), as expected"
else
  check "[INFO] D16 (6.30) did the front end batch a slot's transforms into one dispatch" "reported, not judged" INFO \
        "${batch_pair} batch_max=${batch_max} ${batch_src:-} slot_symbols=${slot_syms:-?}: the front end DID defer  <-- compare V1 and the [ul_gpu_lane] front-end window against the control legs; ${batch_pair} is dispatches/transforms"
fi
occ_line=$(grep -a "queue occupancy (Q9-F3): commits=" "$LEGF" | tail -1)
occ_hole=$(grep -a "hole .*-> next label" "$LEGF" | head -1)
fe_line=$(grep -a "dft carried blocks (Q9-F2)" "$LEGF" | tail -1)
fe_wait=$(grep -a "dft carried deposit ->GPU start" "$LEGF" | tail -1)
if [ -z "${occ_line:-}" ] && [ -z "${fe_line:-}" ]; then
  check "[INFO] D12 (Q9-F2/F3) was the device idle, and what were the front-end blocks doing" "reported, not judged" INFO \
        "no 'queue occupancy (Q9-F3)' and no 'dft carried blocks (Q9-F2)' line - a leg flown before 6.19 cannot say"
else
  check "[INFO] D12 (Q9-F2/F3) was the device idle, and what were the front-end blocks doing" "reported, not judged" INFO \
        "${occ_line:-<no Q9-F3 line: the occupancy probe is off (OCUDU_METAL_GPU_TIME=1 turns it on)>}${occ_hole:+  ||  ${occ_hole}}  ||  ${fe_line:-<no Q9-F2 line>}${fe_wait:+  ||  ${fe_wait}}}"
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
