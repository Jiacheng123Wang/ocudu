#!/usr/bin/env bash
# L1 offline harness for the D1 hand-over (design document 5.9.37): the A/B and its falsification arms.
#
# The question this answers WITHOUT a radio, a UE or a capture: does handing a slot's block over
# uncommitted produce the very same grid the front end would have committed itself? The tool is the
# grid's consumer (grid_ready_hook::wait) and it asserts on the hand-over's own counters, so an arm
# that exercised nothing cannot pass (see ul_chain_replay.cpp, "[l1_handover]").
#
#   ref    hand-over OFF (the reference: the front end commits its own block)
#   cand   hand-over ON  (OCUDU_DFT_RELEASE_BLOCK=1)
#   drop   hand-over ON, consumer wait REMOVED on purpose -> MUST differ (proves cand's pass means something)
#   nogrid hand-over ON, fresh grid per slot instead of one reused -> the same claim, easier shape
#
# Usage: doc_chinese/phy_pipeline_gpu/wip/l1_handover_arms.sh [slots] [workdir]
set -u

SLOTS="${1:-8}"
WORK="${2:-/tmp/l1_handover}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
TOOL="$REPO/build/lib/phy/upper/channel_processors/metal/ul_chain_replay"

if [[ ! -x "$TOOL" ]]; then
  echo "missing $TOOL - build it: cmake --build build --target ul_chain_replay" >&2
  exit 2
fi

rm -rf "$WORK"
mkdir -p "$WORK"

# One arm: run the tool, keep its report, and fail loudly if the tool itself refused the run.
run_arm() {
  local name="$1"; shift
  local log="$WORK/$name.log"
  ( cd "$WORK" && "$@" ) >"$log" 2>&1
  local rc=$?
  local line
  line="$(grep -m1 '^\[l1_handover\]' "$log")"
  printf '%-6s rc=%d  %s\n' "$name" "$rc" "${line:-<no [l1_handover] report>}"
  return $rc
}

BASE=(--dft --dft-metal --device-grid --synth "$SLOTS" --reuse-grid --synth-first-slot 1)

# The reference: the front end commits and waits for its own block, as it always did.
run_arm ref "$TOOL" "${BASE[@]}" --out "$WORK/ref" || echo "  ^ ref arm FAILED" >&2
# The candidate: the block is handed over uncommitted and the tool, as the consumer, produces it.
OCUDU_DFT_RELEASE_BLOCK=1 OCUDU_GPU_STRICT=1 \
  run_arm cand "$TOOL" "${BASE[@]}" --out "$WORK/cand" || echo "  ^ cand arm FAILED" >&2
# Falsification (i): the consumer does not wait -> it reads a grid nobody produced.
OCUDU_DFT_RELEASE_BLOCK=1 OCUDU_GPU_STRICT=1 OCUDU_L1_DROP_CONSUMER_WAIT=1 \
  run_arm drop "$TOOL" "${BASE[@]}" --out "$WORK/drop" || echo "  ^ drop arm FAILED (expected: it reads unproduced grids)" >&2
# Falsification (ii): the same claim on the easier shape (a fresh allocation per slot).
OCUDU_DFT_RELEASE_BLOCK=1 OCUDU_GPU_STRICT=1 \
  run_arm nogrid "$TOOL" --dft --dft-metal --device-grid --synth "$SLOTS" --synth-first-slot 1 \
    --out "$WORK/nogrid" || echo "  ^ nogrid arm FAILED" >&2
# Falsification (iii): the SLOT-BASIS trap. The hand-over key is (storage, slot) and its two halves are
# written by two different pieces of code, so a consumer whose slot numbering differs from the producer's is
# told "nothing pending" and reads the grid anyway (the hook FAILS OPEN). The tool must go RED here - this
# arm is what says the harness would notice if its own key were ever wrong.
OCUDU_DFT_RELEASE_BLOCK=1 OCUDU_GPU_STRICT=1 OCUDU_L1_CONSUMER_SLOT_SKEW=1 \
  run_arm skew "$TOOL" "${BASE[@]}" --out "$WORK/skew"; SKEW_RC=$?

# The comparison: bit for bit, over every dumped grid of every slot.
compare() {
  local a="$1" b="$2"
  local differ=0 files=0
  for f in "$WORK/$a"_*_dft.bin; do
    [[ -e "$f" ]] || continue
    files=$((files + 1))
    local other="$WORK/$b${f#"$WORK/$a"}"
    if ! cmp -s "$f" "$other"; then
      differ=$((differ + 1))
      [[ $differ -le 3 ]] && echo "    differs: $(basename "$f") vs $(basename "$other") ($(cmp "$f" "$other" 2>&1 | head -1))"
    fi
  done
  printf '%-18s files=%-4d differing=%d\n' "$a vs $b" "$files" "$differ"
  return $differ
}

echo
echo "== grid dumps, bit for bit =="
compare cand  ref;  CAND_RC=$?
compare drop  ref;  DROP_RC=$?
compare nogrid ref; NOGRID_RC=$?
compare skew  ref;  SKEW_DIFF=$?

# NON-VACUITY: "the two arms agree" means nothing if both are empty. A grid the front end never wrote is
# all zeros, and two all-zero dumps compare equal - which is how an offline check can pass while the thing
# it measures does nothing at all (5.9.19). Every reference dump must therefore carry real content.
echo
echo "== the reference must not be empty =="
EMPTY_RC=0
for f in "$WORK"/ref_*_dft.bin; do
  [[ -e "$f" ]] || continue
  # LC_ALL=C: the dumps are raw binary, and a UTF-8 locale makes tr refuse the bytes outright (which reads
  # as "empty" and would fail every dump).
  if [[ "$(LC_ALL=C tr -d '\000' <"$f" | wc -c | tr -d ' ')" == "0" ]]; then
    echo "    EMPTY: $(basename "$f") is all zeros - the front end wrote nothing, every comparison above is void"
    EMPTY_RC=$((EMPTY_RC + 1))
  fi
done
if [[ $EMPTY_RC -eq 0 ]]; then
  echo "PASS: every reference dump carries non-zero grid content"
else
  echo "FAIL: $EMPTY_RC reference dump(s) are all zeros"
fi

echo
echo "== verdict =="
rc=0
if [[ $CAND_RC -ne 0 ]]; then
  echo "FAIL: the hand-over changed the grid ($CAND_RC slot dumps differ) - D1 is broken"
  rc=1
else
  echo "PASS: the hand-over produced byte-identical grids (cand == ref)"
fi
if [[ $DROP_RC -eq 0 ]]; then
  echo "FAIL: dropping the consumer wait changed NOTHING - this harness cannot judge the hand-over"
  rc=1
else
  echo "PASS: dropping the consumer wait differs in $DROP_RC slot dump(s) - the harness can fail"
fi
if [[ $NOGRID_RC -ne 0 ]]; then
  echo "FAIL: the hand-over is wrong when every slot gets a fresh grid"
  rc=1
else
  echo "PASS: a fresh grid per slot is byte-identical too"
fi
# The slot-basis trap (memo 4.2): the skewed arm must BOTH differ and be refused by the tool's own
# assertion. Differing alone would be weaker - it would leave the tool reporting success on a run that
# judged nothing.
if [[ $SKEW_DIFF -eq 0 ]]; then
  echo "FAIL: a skewed consumer slot still reproduced the reference - the key is not being checked at all"
  rc=1
elif [[ $SKEW_RC -eq 0 ]]; then
  echo "FAIL: the skewed consumer slot changed the grid but the tool still reported SUCCESS - a slot-numbering mismatch must go red, not silent"
  rc=1
else
  echo "PASS: a skewed consumer slot was refused by the tool (rc=$SKEW_RC) and differs in $SKEW_DIFF dump(s)"
fi
if [[ $EMPTY_RC -ne 0 ]]; then
  rc=1
fi
exit $rc
