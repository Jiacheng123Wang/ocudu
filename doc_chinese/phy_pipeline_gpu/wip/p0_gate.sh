#!/usr/bin/env bash
# P0 gate (doc_chinese/phy_latency/03_p0_instrumentation.md): read the P0 instruments out of a leg, and
# judge them against the criteria that were written down BEFORE the readings existed.
#
# It READS LOG FILES ONLY - it never starts a gNB, a replay or anything that touches the GPU, so it is
# safe to run while the operator is flying a leg.
#
# usage:
#   bash p0_gate.sh <leg-label|path>                      # P0-6 (always) + P0-1 (if the leg is a split arm)
#   bash p0_gate.sh <split-leg> --vs=<factory-leg>        # + the "+-10% of the factory arm's merged_hop" criterion
#
# Criteria (registered in 03_p0_instrumentation.md (1) and (2)):
#   A1  the two [ul_lane_exec] lines are present, and the value they report equals the executor's own
#       max_concurrency (self-consistency: the resolved value and the shape must agree)
#   A2  the shape follows the rule: <= 1 must read STRAND, > 1 must read "task fork limiter"
#   B1  a split arm's `busy split` gains a `dft=` token whose cbs/lane is 1.00 (the front end's buffer is
#       its own submission), and a factory arm's does NOT gain one
#   B2  (only with --vs) dft + (the rest) is within +-10% of the factory arm's merged_hop
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
knob=$(grep -aE "^knob +: OCUDU_LANE_DIAG_SPLIT=" "$LEGF" | tail -1 | sed 's/.*=//')
if [ -z "$split" ]; then
  check "B1 the split arm gains a dft= token (factory arm does not)" "dft present iff the knob is on" RED "no 'busy split' line"
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
knob_ph=$(grep -aE "^knob +: OCUDU_UL_PHASE_SEGMENTS=" "$LEGF" | tail -1 | sed 's/.*=//')
n_phase=$(grep -ac "ul_time_frequency" "$LEGF")
n_lane=$(grep -aoE "\[ul_gpu_lane\] residency samples=[0-9]+" "$LEGF" | tail -1 | grep -oE "[0-9]+$")
if [ -n "${knob_ph:-}" ] && [ "${knob_ph:-0}" != "0" ]; then
  check "C1 a leg that declares the phase segments actually records them" "OCUDU_UL_PHASE_SEGMENTS=1 -> segments present" \
        "$([ "${n_phase:-0}" -gt 0 ] && echo PASS || echo FAIL)" \
        "knob=$knob_ph, phase-segment lines=$n_phase"
else
  check "C1 a leg that declares the phase segments actually records them" "OCUDU_UL_PHASE_SEGMENTS=1 -> segments present" INFO \
        "not a phases leg (knob unset) - C1 does not apply"
fi
# The pairing itself is REPORTED, never judged: no threshold for "paired" was ever registered, and
# inventing one inside a gate is how a criterion becomes whatever the last person wanted (5.9.101).
check "[INFO] P0-5: the two probes' populations (not judged - no threshold registered)" \
      "phase samples vs lane residency samples" INFO \
      "phase-segment lines=${n_phase:-0}, lane residency samples=${n_lane:-<none>}$([ -n "${n_lane:-}" ] && [ "${n_lane:-0}" != "0" ] && awk -v a="${n_phase:-0}" -v b="$n_lane" 'BEGIN{printf "  (ratio %.3f)", a/b}')"

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
