#!/usr/bin/env bash
# Every local gate of a leg, in ONE command and STRICTLY SERIAL.
#
# Why serial is not a style choice: two GPU gates at once on this machine produce FALSE mismatches
# (the design doc has the captures that did it - syn008_6 / syn007_6), and the two alternate build
# trees recompile the .metal sources IN PLACE in the source tree, so building them while a Metal test
# is running makes the test load a half-written kernel. Both mistakes were made; the runner is the
# fix. The order below is the one the handoff memos prescribe:
#
#   1. three capture nets   (run_ab_all.sh   - strict / bounded / CPU, against the archived reference)
#   2. the lane's order A/B (ab_fused_lane.sh - event / wait / burst must be byte-identical)
#   3. the seven capture gates (run_gates.sh - incl. ratdev, the device-vs-host ratio byte identity)
#   4. the Metal engine self-tests (6 binaries, one of them the estimator's own 13 tests)
#   5. ctest -L phy
#   6. the two alternate CONFIG builds (nostats / nometal) - each must reach gnb with exit=0
#
# The Ubuntu gate (build + ctest -L phy on 192.168.100.131) is NOT here: it pins the tree that is
# pushed, so it belongs after the commit, not before it.
#
# usage: bash run_leg_gates.sh [label] [nof-captures]
set -u
LABEL=${1:-$(date +%m%d_%H%M)}
N=${2:-27}
ROOT=/Users/jiachengwang/dev/ocudu
W=$ROOT/doc_chinese/full_gpu_chain/wip
REF=$ROOT/doc_chinese/work_tmp/ref/ul_chain_replay_s7g5_ref
OUT=$ROOT/doc_chinese/work_tmp/gates/$LABEL
mkdir -p "$OUT"
cd "$ROOT" || exit 2

if [ ! -x "$REF" ]; then echo "missing reference binary: $REF" >&2; exit 2; fi
if pgrep -x gnb >/dev/null 2>&1; then echo "a gnb is still running: kill it first (the leg would be void)" >&2; exit 2; fi

rc_all=0
run()
{
  local name=$1; shift
  echo "=== $name start $(date +%H:%M:%S)"
  "$@" > "$OUT/$name.log" 2>&1
  local rc=$?
  echo "=== $name rc=$rc $(date +%H:%M:%S)"
  tail -6 "$OUT/$name.log"
  [ $rc -eq 0 ] || rc_all=1
}

# ---------------------------------------------------------------------------------------------
# STEP 0, and the reason this script exists in this shape: `cmake --build build` builds `all`, and
# the binaries the capture nets use are NOT in it (ul_chain_replay, the Metal engine self-tests).
# A leg that edits lib/ and only runs `all` therefore leaves the OLD test binaries behind while the
# .metal sources - which the two builds at the end of this script regenerate IN PLACE in the source
# tree - move on, and the nets then run a binary whose host-side structs no longer match the
# metallib it loads. Measured: exactly that produced `sinr=inf` and 0/27 byte-identical on every
# metal-LDPC route, which reads like a catastrophic regression and is nothing but a stale binary.
# The Metallib is regenerated here too, because those same two builds rewrite it (with identical
# bytes - verified - but a leg must not depend on that).
# ---------------------------------------------------------------------------------------------
echo "=== 0_build start $(date +%H:%M:%S)"
cmake --build build -j"$(sysctl -n hw.ncpu)" > "$OUT/0_build.log" 2>&1 && \
cmake --build build --target ul_chain_replay port_channel_estimator_metal_mmse_unit_test \
      dft_processor_metal_unit_test ofdm_demodulator_metal_batch_test \
      demodulation_mapper_metal_unit_test channel_equalizer_metal_unit_test \
      baseband_gateway_buffer_metal_smoke_test -j"$(sysctl -n hw.ncpu)" >> "$OUT/0_build.log" 2>&1
rc=$?; echo "=== 0_build rc=$rc $(date +%H:%M:%S)"; tail -3 "$OUT/0_build.log"
[ $rc -eq 0 ] || { echo "LEG_GATES_RC=1 (build)"; exit 1; }

run 1_ab_all       bash "$W/run_ab_all.sh" "$REF" "$N"
run 2_fused_lane   bash "$W/ab_fused_lane.sh" "$N"
run 3_capture_gates bash "$W/run_gates.sh" "$OUT/capture_gates"
run 4_metal_engine bash "$W/run_metal_engines.sh" build

echo "=== 5_ctest_phy start $(date +%H:%M:%S)"
( cd build && ctest -L phy --output-on-failure ) > "$OUT/5_ctest_phy.log" 2>&1
rc=$?; echo "=== 5_ctest_phy rc=$rc $(date +%H:%M:%S)"; tail -4 "$OUT/5_ctest_phy.log"
[ $rc -eq 0 ] || rc_all=1

echo "=== 6_build_nostats start $(date +%H:%M:%S)"
( cmake -S . -B /tmp/build_nostats -DCMAKE_BUILD_TYPE=Release &&
  cmake --build /tmp/build_nostats --target gnb -j 10 ) > "$OUT/6_build_nostats.log" 2>&1
rc=$?; echo "=== 6_build_nostats rc=$rc $(date +%H:%M:%S)"; tail -4 "$OUT/6_build_nostats.log"
[ $rc -eq 0 ] || rc_all=1

echo "=== 7_build_nometal start $(date +%H:%M:%S)"
( cmake -S . -B /tmp/build_nometal -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_METAL_CHEST=OFF -DENABLE_METAL_LDPC=OFF -DENABLE_METAL_DFT=OFF \
    -DENABLE_METAL_EQUALIZER=OFF -DENABLE_METAL_DEMODULATION=OFF &&
  cmake --build /tmp/build_nometal --target gnb -j 10 ) > "$OUT/7_build_nometal.log" 2>&1
rc=$?; echo "=== 7_build_nometal rc=$rc $(date +%H:%M:%S)"; tail -4 "$OUT/7_build_nometal.log"
[ $rc -eq 0 ] || rc_all=1

echo "LEG_GATES_RC=$rc_all"
echo "logs: $OUT"
exit $rc_all
