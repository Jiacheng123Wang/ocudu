#!/usr/bin/env bash
# L1b offline harness: the HOP half of the D1 hand-over (design document 5.9.38).
#
# L1a (l1_handover_arms.sh) judges the FRONT END: it produces a slot's grid and, as the grid's host
# consumer, asks for its production. It never builds a PUSCH receiver, so the other half of the hand-over -
# the hop that ADOPTS the front end's block and appends its own dispatches to the very same command
# buffer - is not in it. Here the capture supplies the PDU CONFIGURATION only and the grid comes from the
# front end, so a real receiver runs on a block the receiving chain really deposited.
#
#   ref        hand-over OFF. The hop reads a grid the front end committed itself.
#   cand       hand-over ON, hop goes FIRST -> it ADOPTS the block (taken>0): D1's headline shape.
#   hostfirst  hand-over ON, the host consumer WAITS first -> the hop MISSES (taken=0) and takes the
#              device-side wait path (grid_devwaited>0).
#   claim      hand-over ON, another consumer CLAIMS the block but does NOT wait -> the hop MISSES with
#              the commit still in flight, the shape in which the device-side wait should matter.
#   claimnowait   the same, with the device-side wait DROPPED (OCUDU_L1_DROP_MISS_WAIT=1).
#
# The criterion: the LLRs of every slot must be identical across all of them. What each arm EXERCISED is
# asserted by the tool itself (see [l1_hop]) - an arm whose counters say it did not take the path it
# meant to is a failed arm, not a passing comparison.
#
# WHAT THIS HARNESS CANNOT DO (measured, design document 5.9.39): it cannot falsify the MISS path's
# device-side wait. `claim` and `claimnowait` differ by exactly that wait (grid_devwaited=16 vs 0) and
# produce identical soft bits and identical estimator scalars - 5 repetitions of 16 slots each. Neither
# does the MISS hop of a two-hop slot (L1_HOP_PDUS=2: exactly one hop adopts it, the other misses). The
# hazard is real but cross-QUEUE - the front end writes the grid on the front-end queue, the hop reads it
# on the backend queue - and on an IDLE machine the commit has long completed before the back-end reader
# runs, so the window is never hit. Falsifying that wait needs a LOADED GPU. §4.3 step 4(ii) stays OPEN.
#
# Usage: doc_chinese/phy_pipeline_gpu/wip/l1_hop_arms.sh [slots] [pdu-capture] [workdir]
set -u

SLOTS="${1:-16}"
PDU="${2:-doc_chinese/work_tmp/corpus/syn001_3}"
WORK="${3:-/tmp/l1_hop}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
TOOL="$REPO/build/lib/phy/upper/channel_processors/metal/ul_chain_replay"

if [[ ! -x "$TOOL" ]]; then
  echo "missing $TOOL - build it: cmake --build build --target ul_chain_replay" >&2
  exit 2
fi
if [[ ! -f "$REPO/$PDU.txt" ]]; then
  echo "missing PDU capture $REPO/$PDU.txt" >&2
  exit 2
fi

rm -rf "$WORK"
mkdir -p "$WORK"

run_arm() {
  local name="$1"; shift
  ( cd "$REPO" && "$@" ) >"$WORK/$name.log" 2>&1
  local rc=$?
  printf '%-12s rc=%d  %s\n' "$name" "$rc" "$(grep -m1 '^\[l1_hop\]' "$WORK/$name.log")"
  grep -m1 '^\[l1_multi\]' "$WORK/$name.log" | sed 's/^/             /'
  return $rc
}

PDUS="${L1_HOP_PDUS:-1}"
ARM=(--metal --device-grid --hop-td "$SLOTS" --hop-pdus "$PDUS")

run_arm ref "$TOOL" "$PDU" --out "$WORK/ref" "${ARM[@]}" || echo "  ^ ref arm FAILED" >&2
OCUDU_DFT_RELEASE_BLOCK=1 OCUDU_GPU_STRICT=1 \
  run_arm cand "$TOOL" "$PDU" --out "$WORK/cand" "${ARM[@]}" || echo "  ^ cand arm FAILED (see [l1_hop])" >&2
OCUDU_DFT_RELEASE_BLOCK=1 OCUDU_GPU_STRICT=1 OCUDU_L1_HOST_FIRST=1 \
  run_arm hostfirst "$TOOL" "$PDU" --out "$WORK/hostfirst" "${ARM[@]}" || echo "  ^ hostfirst arm FAILED (see [l1_hop])" >&2
OCUDU_DFT_RELEASE_BLOCK=1 OCUDU_GPU_STRICT=1 OCUDU_L1_CLAIM_ONLY=1 \
  run_arm claim "$TOOL" "$PDU" --out "$WORK/claim" "${ARM[@]}" || echo "  ^ claim arm FAILED (see [l1_hop])" >&2
OCUDU_DFT_RELEASE_BLOCK=1 OCUDU_GPU_STRICT=1 OCUDU_L1_CLAIM_ONLY=1 OCUDU_L1_DROP_MISS_WAIT=1 \
  run_arm claimnowait "$TOOL" "$PDU" --out "$WORK/claimnowait" "${ARM[@]}" || echo "  ^ claimnowait arm FAILED (see [l1_hop])" >&2

# The two claim arms must differ in EXACTLY the device-side wait: if the dropped one still reports the wait
# as encoded, the arm did not drop anything and the comparison below says nothing about that wait.
rc=0
for pair in "claim:encoded" "claimnowait:dropped"; do
  arm="${pair%%:*}"; want="${pair##*:}"
  got="$(grep -o 'grid_devwaited=[0-9]*' "$WORK/$arm.log" | head -1)"
  printf '%-12s %s (expected: %s)\n' "$arm" "$got" "$want"
  if [[ "$want" == "dropped" && "$got" != "grid_devwaited=0" ]] || [[ "$want" == "encoded" && "$got" == "grid_devwaited=0" ]]; then
    echo "  ^ the arm did not do what its name says - the falsification below is void" >&2
    rc=1
  fi
done

echo
echo "== every slot's soft bits, bit for bit =="
for arm in cand hostfirst claim claimnowait; do
  files=0; differ=0
  for f in "$WORK"/ref_*_llr.bin; do
    [[ -e "$f" ]] || continue
    files=$((files + 1))
    other="$WORK/$arm${f#"$WORK/ref"}"
    cmp -s "$f" "$other" || differ=$((differ + 1))
  done
  if [[ $files -eq 0 ]]; then
    echo "$arm vs ref: NO LLR CAPTURES - the comparison is void"
    rc=1
  elif [[ $differ -ne 0 ]]; then
    echo "$arm vs ref: files=$files differing=$differ  <-- THE MECHANISM CHANGED THE DATA"
    rc=1
  else
    echo "$arm vs ref: files=$files differing=0"
  fi
done

echo
echo "== verdict =="
if [[ $rc -eq 0 ]]; then
  echo "PASS: adopting the front end's block, and missing it, both leave every soft bit unchanged"
else
  echo "FAIL: see above"
fi
echo "OPEN: the device-side wait is NOT falsifiable here. The claim arms differ by exactly that wait"
echo "      (grid_devwaited>0 vs 0) and agree bit for bit, and so does the MISS hop of a two-hop slot"
echo "      (L1_HOP_PDUS=2: exactly one hop adopts, the other misses) - 5.9.39/5.9.40."
echo "      The hazard is real but cross-QUEUE: the front end writes the grid on the front-end queue and"
echo "      the hop reads it on the backend queue, so nothing orders the two except that wait. On an IDLE"
echo "      machine the front end's commit has long completed before the back-end queue reaches the"
echo "      reader, so the window is never hit. Reproducing it needs a LOADED GPU - which is what the air"
echo "      legs have and this harness does not."
# NOTE deliberately NOT compared here: the OCUDU_UL_DUMP *grid* capture (<prefix>_<slot>_<rnti>.bin).
# It is a HOST read of the grid, and in an arm where the hop ADOPTS the block the grid is produced at that
# hop's commit - so the capture reads memory nobody has written yet and differs for reasons that have
# nothing to do with the mechanism (measured: the adopting arm's grid capture differs on every slot while
# its soft bits and estimator scalars are identical). The soft bits and the estimator scalars are the
# stages that are read after the hop's commit, and those are what this harness judges.
exit $rc
