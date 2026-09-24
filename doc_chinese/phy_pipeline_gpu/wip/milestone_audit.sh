#!/usr/bin/env bash
# MILESTONE AUDIT - one entry point for every mechanical acceptance criterion of the fused-lane work.
#
# Why one script: the milestone is declared with a sentence, and a sentence is worth exactly the
# criteria behind it. This line has already retracted one milestone (design document 9.2) and lost
# another 121 tests behind a ctest filter (5.9.114), so the acceptance is packaged as code that can
# be re-run, not as a paragraph that can be re-read.
#
# The rules it inherits from the record:
#   * "cannot read" is RED, never absent (5.9.97);
#   * a check is judged by its NAMES as well as its number, because N is dynamic (pitfall 28);
#   * the byte net is INFORMATION, not a criterion (5.8.x "two nets", corrected in 6.3) - the gate
#     is wip/value_net.py;
#   * every replay-driven step is SERIAL: two ul_chain_replay instances in parallel give wrong
#     results (6.3), so this script never fans out.
#
# usage:
#   bash doc_chinese/phy_pipeline_gpu/wip/milestone_audit.sh                 # everything offline
#   bash doc_chinese/phy_pipeline_gpu/wip/milestone_audit.sh --leg s69-a12-n78
#   bash doc_chinese/phy_pipeline_gpu/wip/milestone_audit.sh --quick         # skip the long arms
#
# NOT included because they need a radio + a UE (they are listed at the end as AIR items):
# the air leg itself, corr_fenced_ab.sh, k1_barrier_slope.sh, ul_load.sh.
set -u

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
W=$ROOT/doc_chinese/phy_pipeline_gpu/wip
LOGDIR=$W/logs
LEG=""
STRESSLEG=""
QUICK=0
for a in "$@"; do
  case "$a" in
    --leg)          shift; LEG=${1:-} ;;
    --leg=*)        LEG=${a#*=} ;;
    --stress-leg)   shift; STRESSLEG=${1:-} ;;
    --stress-leg=*) STRESSLEG=${a#*=} ;;
    --quick) QUICK=1 ;;
  esac
done

# ---------------------------------------------------------------- 0a. TWO REGIMES, TWO SETS OF CRITERIA
# A leg runs in one of two regimes, and they do NOT share their criteria:
#
#   default : no load generator. This is the regime the milestone criteria were written for - the
#             contract, the crossings, cbs/lane, and the A1-2 attribution gate's C5, whose
#             pre-registration is literally "the leg is still valid: contract 8/8, stale=0,
#             crossings 0.00+0.00/hop" (5.9.118 (3)). `stale=0` means no hop crossed 8 ms, the
#             uplink HARQ round trip.
#   stress  : a load generator drove the cell (wip/ul_load.sh, wip/wall_ab.sh). Here `stale > 0` is
#             the PRE-REGISTERED EXPECTATION, not a failure: the wall A/B's criterion R4 reads
#             "stale > 0 => this leg is of the same kind as s70", PASS (5.9.127). The legs that
#             carry the L-1 latency decomposition are exactly these (s78/s80/s82).
#
# Measured 2026-09-24: this script auto-picked the NEWEST leg, which was s82 - the deliberately
# overloaded n78 leg run for the latency decomposition - and reported `stale = 0` and the A1-2 gate
# as FAIL on it. Both were the criterion bound to the wrong regime, not a finding. The defect was
# invisible before because an earlier HEAD's newest leg was a stress leg that happened to read
# stale=0, so the default-regime criterion passed on it by luck.
#
# A regime is a DECLARATION, never a symptom: classifying a leg by its own `stale` reading would make
# the criterion unfalsifiable. Hence the two sources below - and run_leg.sh now writes
# `[leg] regime=` into the leg's own stderr, which is the source of record for every new leg.
#
# A leg driven by a load generator MUST declare itself: pass --regime=stress to wip/run_leg.sh, or
# add its label here WITH the load evidence (for legs that predate that flag).
STRESS_LEGS="
s63-n78 s70-heavy-n78 s72-wall-gpu-n78 s73-wall-cpu-n78 s74-wall-gpu-n78-cold
s75-wall-gpu-n78-hot s76-wall-premerge-n78 s77-wall-cpu-n78 s78-dlcap40-n78 s79-dlcap40-cpu-n78
s80-ulcap40-n78 s81-ulcap40-cpu-n78 s82-phases-heavy-n78
"
# The list above is derived from wip/wall_ab.sh's own header and its 5.9.127 note: "A-1 gpu, the same
# load recipe as s70" (line 3) and "the uplink-load legs (s70..s75) and the downlink-saturated ones
# (s76..s79)" (line 129).
leg_regime() {
  local label=$1 f line s
  f="$LOGDIR/gnb_gpu_$label.log.stderr"
  [ -f "$f" ] || f=$(ls -1t "$LOGDIR"/gnb_*"$label"*.log.stderr 2>/dev/null | head -1)
  if [ -n "${f:-}" ] && [ -f "$f" ]; then
    line=$(grep -aoE '\[leg\] regime=[a-z]+' "$f" | tail -1)
    [ -n "$line" ] && { echo "${line#*=}"; return; }
  fi
  for s in $STRESS_LEGS; do case "$label" in *"$s"*) echo stress; return;; esac; done
  echo default
}
leg_labels_newest_first() {
  ls -1t "$LOGDIR"/gnb_gpu_*.log.stderr 2>/dev/null | xargs -I{} basename {} .log.stderr | sed 's/^gnb_gpu_//'
}
# An explicit --leg that names a STRESS leg is redirected, not honoured: honouring it would bind the
# default-regime criteria to the one regime where they were pre-registered to read the other way.
if [ -n "$LEG" ] && [ "$(leg_regime "$LEG")" = stress ]; then
  echo "NOTE: --leg=$LEG is a declared STRESS-regime leg. It is audited as the stress leg; the" >&2
  echo "      default-regime criteria (stale = 0, A1-2 5/5) are bound to the newest DEFAULT leg." >&2
  [ -z "$STRESSLEG" ] && STRESSLEG=$LEG
  LEG=""
fi
if [ -z "$LEG" ] || [ -z "$STRESSLEG" ]; then
  while read -r cand; do
    [ -n "$cand" ] || continue
    if [ "$(leg_regime "$cand")" = stress ]; then
      [ -z "$STRESSLEG" ] && STRESSLEG=$cand
    else
      [ -z "$LEG" ] && LEG=$cand
    fi
    [ -n "$LEG" ] && [ -n "$STRESSLEG" ] && break
  done <<<"$(leg_labels_newest_first)"
fi

T=$(mktemp -d)

# ---------------------------------------------------------------- 0a. RUN ALONE, OR NOT AT ALL
# Most of this script drives one GPU replay, and this line's own record (6.3) says two replay instances in
# parallel give WRONG results. Measured 2026-09-24, by accident: two of these audits overlapped for 1.7
# minutes and produced four false failures - the byte net read 88 bytes over 14 captures, ab_dumps read
# 1993 differing bytes where it reads 0, all four L1b hop arms reported differences, and the MMSE rate arm
# produced a "landmine" outlier at drift 395. Every one of them vanished when the same audit ran alone.
# A gate that can be run twice is a gate that will be run twice, so it refuses instead.
LOCK=${TMPDIR:-/tmp}/ocudu_milestone_audit.lock
if ! mkdir "$LOCK" 2>/dev/null; then
  owner=$(cat "$LOCK/pid" 2>/dev/null || echo "")
  if [ -n "$owner" ] && kill -0 "$owner" 2>/dev/null; then
    echo "REFUSING to run: another milestone audit is already running (pid $owner)." >&2
    echo "  Two of these in parallel drive the same GPU replay and report FALSE failures" >&2
    echo "  (measured 2026-09-24: byte net 88 bytes/14 captures, ab_dumps 1993 bytes, L1b 4/4 arms," >&2
    echo "  a 'landmine' outlier at drift 395 - all of which disappear when run alone)." >&2
    exit 4
  fi
  rm -rf "$LOCK"          # the previous run died without cleaning up
  mkdir "$LOCK" || exit 4
fi
echo $$ >"$LOCK/pid"
trap 'rm -rf "$T" "$LOCK"' EXIT

rows=()
# check <name> <expected text> <verdict: PASS|FAIL|RED> <detail>
check() { rows+=("$1|$2|$3|$4"); }

# ---------------------------------------------------------------- 0b. SAY WHAT IS RUNNING
# The report is printed in one block at the END, so before this line existed a healthy run looked
# IDENTICAL to a hung one for minutes: measured 2026-09-24, a user asked whether the gate had
# deadlocked while it was simply 1:53 into the 47-capture GPU replay (the lock file and `ps` were the
# only way to tell). Two of these criteria drive real work - value_net's 47 captures and the 193-test
# PHY suite - so progress goes to stderr as it happens, and the report stays as it was.
stage() { printf '  [%-5s] %s\n' "$1" "$2" >&2; }
stage start "milestone audit on $(git rev-parse --short=10 HEAD 2>/dev/null || echo '<no git>'): ~3 minutes, \"criterion\" lines below are PROGRESS, the verdicts come at the end"

# A leg's OWN commit, against HEAD. wip/run_leg.sh refuses to start when the binary's stamp differs from
# HEAD ("a leg run now would be evidence about $STAMP, not about the commit you are testing"), so a leg
# is evidence about the commit it ran - but that fact lived only in the console and in the .stdout
# banner, and nothing here checked it: this script's "binary stamp == HEAD" row is about build/hashes.h,
# i.e. about the CURRENT build, not about the leg. A leg whose commit differs is still evidence IF the
# diff between it and HEAD touches no code. Measured 2026-09-24: the newest leg s82 ran 33de116ce3 while
# HEAD was 4c532a9fb1, and 33de116ce3..HEAD changes 4 files, all under doc_chinese/phy_latency/ - so it
# is evidence about HEAD's PHY behaviour, and now says so itself instead of a human asserting it.
leg_commit_check() {   # <label> <leg .stderr path> <kind>
  local label=$1 f=$2 kind=$3 stdout legc paths nfiles ncode
  stdout=${f%.stderr}.stdout
  legc=$(grep -aoE '\(commit [0-9a-f]{7,40}\)' "$stdout" 2>/dev/null | head -1 | grep -oE '[0-9a-f]{7,40}')
  if [ -z "$legc" ]; then
    check "$kind leg $label: the commit it ran, vs HEAD" "equal, or a diff that touches no code" RED \
          "no commit banner in $(basename "$stdout") - cannot tie this leg to a commit"
    return
  fi
  if [ "$legc" = "$head10" ]; then
    check "$kind leg $label: the commit it ran, vs HEAD" "equal, or a diff that touches no code" PASS \
          "the leg ran $legc = HEAD"
    return
  fi
  if ! git cat-file -e "${legc}^{commit}" 2>/dev/null; then
    check "$kind leg $label: the commit it ran, vs HEAD" "equal, or a diff that touches no code" RED \
          "the leg ran $legc, which is not a commit in this repository"
    return
  fi
  paths=$(git diff --name-only "$legc"..HEAD 2>/dev/null)
  nfiles=$(printf '%s\n' "$paths" | grep -c .)
  ncode=$(printf '%s\n' "$paths" | grep -cE '^(lib/|include/|apps/|tests/)')
  check "$kind leg $label: the commit it ran, vs HEAD" "equal, or a diff that touches no code" \
        "$([ "${ncode:-0}" = "0" ] && echo PASS || echo FAIL)" \
        "leg ran $legc; $legc..HEAD changes ${nfiles:-0} file(s), ${ncode:-0} of them under lib/include/apps/tests$([ "${ncode:-0}" != "0" ] && echo '  <- this leg is NOT evidence about HEAD')"
}

# ---------------------------------------------------------------- 0. the binary under test
cd "$ROOT"
stamp=$(grep -oE '[0-9a-f]{10}' build/hashes.h 2>/dev/null | head -1)
head10=$(git rev-parse --short=10 HEAD 2>/dev/null)
check "binary stamp == HEAD" "$head10" \
      "$([ -n "$stamp" ] && [ "$stamp" = "$head10" ] && echo PASS || echo "$([ -z "$stamp" ] && echo RED || echo FAIL)")" \
      "stamp=${stamp:-<unreadable>} HEAD=${head10:-<unreadable>}  (a leg on a stale binary is evidence about other code)"

sw=$(grep -cE "^(ENABLE_METAL_STATS|ENABLE_FLOW_PROBES|ENABLE_CE_TIME|ENABLE_UL_CAPTURE):BOOL=ON" build/CMakeCache.txt 2>/dev/null)
check "configure has the 4 debug-aid switches ON (6.1)" "4" \
      "$([ "${sw:-0}" = "4" ] && echo PASS || echo "$([ -z "${sw:-}" ] && echo RED || echo FAIL)")" \
      "found ${sw:-0} of 4 in build/CMakeCache.txt"

# ---------------------------------------------------------------- 1. the VALUE gate (and the demoted byte net)
# A red from this gate is RE-RUN ONCE, alone, and the detail says which attempt produced the verdict. It is
# a GPU replay, and the project's own record (6.3) says two replay instances in parallel give wrong
# results - on 2026-09-24 the audit's first run read `problems=3` and every re-run (four of them, the last
# three back-to-back with no other process on the GPU) read 0. "Single green is zero evidence" cuts both
# ways: a single RED is not evidence either until it repeats.
vrc=1
vattempt=0
for attempt in 1 2; do
  vattempt=$attempt
  stage run "value_net.py: 47 captures through the GPU replay (the slowest criterion, ~1-2 min)"
  python3 $W/value_net.py >"$T/value" 2>&1; vrc=$?
  vline=$(grep -E "^captures=" "$T/value" | tail -1)
  [ "$vrc" = 0 ] && break
done
check "value_net.py: captures=47 problems=0 (the gate)" "47 / 0" \
      "$([ "$vrc" = 0 ] && echo PASS || echo "$([ $vrc = 2 ] && echo RED || echo FAIL)")" \
      "${vline:-<unreadable>} (exit $vrc, attempts=$vattempt)"

python3 $W/value_net.py --self-test >"$T/vself" 2>&1
vs=$(grep -oE "self-test: [0-9]+/[0-9]+" "$T/vself" | tail -1)
check "value_net --self-test: it can SEE the two historical defects" "8/8" \
      "$([ "$vs" = "self-test: 8/8" ] && echo PASS || echo "$([ -z "$vs" ] && echo RED || echo FAIL)")" \
      "${vs:-<unreadable>}"

# INFORMATION, not a criterion: how many bytes this tree moved against the archived baseline.
stage run "byte net vs the archive (INFO only)"
bash $W/neutral_vs_baseline.sh >"$T/byte" 2>&1
bline=$(grep -E "^files-compared=" "$T/byte" | tail -1)
check "[INFO] byte net vs the archive (NOT a criterion since 5.8.x)" "a stable number, read it" "INFO" \
      "${bline:-<unreadable>}  (131 was the recorded, stable value; 6.3 corrected 2026-09-23)"

# ---------------------------------------------------------------- 2. the A/B arms and the cross-hop net
#
# THE FLAKE RULE (6.5), applied for real. This file already carried a criterion NAMED
# "port_channel_estimator_metal_mmse_unit_test (6.5 flake rule: rerun once if red)" - and the script
# never re-ran anything: the rule existed only in the label. Two criteria here are known to read red
# occasionally with no code change, so the re-run is now code:
#
#   * the MMSE unit test (registered as the 6.5 flake);
#   * the ab_dumps historical arm. Measured 2026-09-24: it read 0 differing bytes in 14 consecutive
#     runs AND in 468 existence-verified file comparisons of a hand-run replication (4 rounds x 27
#     captures x 4 dumps), but read 551 bytes once inside a full audit and 2728 bytes once in a loop
#     run immediately after heavy GPU work. Not reproducible on demand; both sides are individually
#     deterministic in isolation (36 A x B cross-pairs on one capture, all identical). The two
#     non-zero readings were adjacent to heavy GPU activity, which is the shape 5.8.20 (4) already
#     registers for this host ("the absolute numbers are mode-dependent, the ratios are not").
#
# A criterion that can go red without a code change is not evidence until it repeats, and the honest
# form is "re-run once, and KEEP THE FIRST READING IN THE DETAIL" - not a silent retry that launders a
# flake into a green. So: rerun_once() runs the arm again when the first read is non-zero, and the
# detail always carries the first reading.
ab_arm() {   # <env A> <env B> <outfile>  -> echoes the summed differing bytes
  bash $W/ab_dumps.sh "$1" "$2" >"$3" 2>&1
  grep -E "_h\.bin|\.bin|_ce\.txt" "$3" | awk '{s+=$NF} END {print s+0}'
}
ab_check() {   # <name> <expected> <env A> <env B> <tag>
  local name=$1 exp=$2 ea=$3 eb=$4 tag=$5 first second
  first=$(ab_arm "$ea" "$eb" "$T/${tag}")
  if [ -n "$first" ] && [ "$first" = "0" ]; then
    check "$name" "$exp" PASS "sum of differing bytes over the 3 dumps = 0"
  else
    second=$(ab_arm "$ea" "$eb" "$T/${tag}r")
    check "$name" "$exp" \
          "$([ -n "$second" ] && [ "$second" = "0" ] && echo PASS || echo "$([ -z "$second" ] && echo RED || echo FAIL)")" \
          "sum of differing bytes over the 3 dumps = ${second:-<unreadable>}    [6.5 flake rule: the FIRST read was ${first:-<unreadable>}; this arm is known to read red occasionally with no code change - see the block comment above]"
  fi
}
check_rerun() {   # <name> <command...> : run, and on red run once more (the MMSE unit test's 6.5 rule)
  local name=$1; shift
  local first second
  first=$("$@" 2>&1 | grep -E "All tests PASSED|FAILED" | tail -1)
  if [ -n "$first" ] && echo "$first" | grep -q "All tests PASSED"; then
    check "$name" "All tests PASSED" PASS "$first"
  else
    second=$("$@" 2>&1 | grep -E "All tests PASSED|FAILED" | tail -1)
    check "$name" "All tests PASSED" \
          "$(echo "$second" | grep -q "All tests PASSED" && echo PASS || echo "$([ -z "$second" ] && echo RED || echo FAIL)")" \
          "${second:-<unreadable>}    [6.5 flake rule: the FIRST read was ${first:-<unreadable>}]"
  fi
}

stage run "ab_dumps: the two replay arms (2 x GPU replay, each re-run once if red)"
ab_check "ab_dumps: P1 fused route vs its one-line rollback" "0 differing bytes" \
         "OCUDU_CE_EDGE_FUSE=0" "" "ab1"
ab_check "ab_dumps: historical arm, both pinned to one sigma2 source" "0 differing bytes" \
         "OCUDU_CE_TAIL_DEV=0 OCUDU_CE_HOST_SCALARS=1" "OCUDU_CE_HOST_SCALARS=1" "ab2"

R=build/lib/phy/upper/channel_processors/metal/ul_chain_replay
C=doc_chinese/work_tmp/corpus/syn004_4
$R $C --metal --repeat 1  --out "$T/a" >/dev/null 2>&1
$R $C --metal --repeat 20 --out "$T/b" >/dev/null 2>&1
fa=$(ls "$T"/a*_ce.txt 2>/dev/null | head -1); fb=$(ls "$T"/b*_ce.txt 2>/dev/null | head -1)
if [ -n "$fa" ] && [ -n "$fb" ]; then
  xh=$(cmp -l "${fa%_ce.txt}_h.bin" "${fb%_ce.txt}_h.bin" 2>/dev/null | wc -l | tr -d ' ')
  xl=$(cmp -l "${fa%_ce.txt}_llr.bin" "${fb%_ce.txt}_llr.bin" 2>/dev/null | wc -l | tr -d ' ')
  check "cross-hop stability (6.4): repeat 1 vs repeat 20" "0 / 0" \
        "$([ "$xh" = 0 ] && [ "$xl" = 0 ] && echo PASS || echo FAIL)" "h=${xh} llr=${xl}"
else
  check "cross-hop stability (6.4): repeat 1 vs repeat 20" "0 / 0" RED "no dumps produced by ul_chain_replay"
fi

$R doc_chinese/work_tmp/corpus/syn004_4 --metal --repeat 20 --out "$T/x" 2>&1 >/dev/null \
  | grep -a "crossings" -A1 >"$T/ss" 2>&1
ss=$(grep -oE "= [0-9.]+ read\(s\) \+ [0-9.]+ write\(s\) per hop" "$T/ss" | tail -1)
check "6.2 steady-state crossings on the replay: 0.00 + 0.00 per hop" "0.00 + 0.00" \
      "$([ "$ss" = "= 0.00 read(s) + 0.00 write(s) per hop" ] && echo PASS || echo "$([ -z "$ss" ] && echo RED || echo FAIL)")" \
      "${ss:-<unreadable>}"

# ---------------------------------------------------------------- 3. the offline arms (long: skipped by --quick)
if [ "$QUICK" = 0 ]; then
  stage run "L1a hand-over arms (5 arms)"
  bash $W/l1_handover_arms.sh 32 >"$T/l1a" 2>&1
  n=$(grep -c '^PASS' "$T/l1a")
  check "L1a hand-over arms: 5 PASS" "5" \
        "$([ "$n" = "5" ] && echo PASS || echo FAIL)" "PASS lines = $n"

  stage run "L1b hop arms (4 arms)"
  bash $W/l1_hop_arms.sh 16 >"$T/l1b" 2>&1
  n=$(grep -c "differing=0" "$T/l1b")
  bad=$(grep -c "differing=[1-9]" "$T/l1b")
  check "L1b hop arms: 4 arms, all differing=0" "4 / 0" \
        "$([ "$n" = "4" ] && [ "$bad" = "0" ] && echo PASS || echo FAIL)" "differing=0 x$n, differing>0 x$bad"

  stage run "edge-block arms (default 6 green / =0 6 red)"
  bash $W/edge_block_arms.sh 6 >"$T/edge" 2>&1
  # The arm harness drives the MMSE unit test, which has a REGISTERED ~10% SIGBUS flake (6.5), and the
  # mix of red criteria varies run to run (5.9.95 5: usually (d) "0 of 480 hops waited", occasionally
  # Test 9/Test 3). So: judge on the stable parts (green "PASS x6", red "[rc: rc=255 x6]"), and apply
  # the project's own flake rule - rerun ONCE if the summary lines are missing, and say that you did.
  edge_attempts=0
  for attempt in 1 2; do
    bash $W/edge_block_arms.sh 6 >"$T/edge" 2>&1
    g=$(grep -cE "default   \(green\): PASS x6" "$T/edge")
    r=$(grep -cE "rollback  \(red\): .*\[rc: rc=255 x6\]" "$T/edge")
    edge_attempts=$attempt
    [ "$g" = "1" ] && [ "$r" = "1" ] && break
  done
  check "edge-block guard: default 6/6 green AND =0 arm 6/6 red" "green + red" \
        "$([ "$g" = "1" ] && [ "$r" = "1" ] && echo PASS || echo "$([ "$g" = 0 ] && [ "$r" = 0 ] && echo RED || echo FAIL)")" \
        "green-line x$g, red-line(any criterion, rc=255, x6) x$r, attempts=$edge_attempts"

  stage run "MMSE landmine rate (10 sweeps)"
  bash $W/mmse_outlier_rate.sh 10 >"$T/rate" 2>&1
  rl=$(grep -E "arm default" "$T/rate" | tail -1)
  check "MMSE landmine rate (smoke, 10 sweeps): 0 failing sweeps" "failing_sweeps=0/10" \
        "$(echo "$rl" | grep -q "failing_sweeps=0/10" && echo PASS || echo "$([ -z "$rl" ] && echo RED || echo FAIL)")" \
        "${rl:-<unreadable>}  (the registered 22% baseline needs ~20/arm to resolve; 20+ is the real read)"
fi

# ---------------------------------------------------------------- 4. the test corpus
# THE GATE IS A LABEL, NOT A NAME REGEX (audit 2026-09-23). The name filter this script used to carry had
# failed twice: 5.9.114 lost 121 tests to a missing `pusch`, and this audit found three structural holes
# in it - (1) `ctest -R` is CASE-SENSITIVE: the same pattern case-insensitively selects 466 tests, so
# `CsiReportPuschHelpersTest`, `PxschChain` and `LowerPhy*` are invisible to it; (2) `o_du` is a
# SUBSTRING TRAP: 19 of the 157 are `sent_to_du` / `no_dus` / `zer*o_du*ration_is_no_op`, i.e. the gate
# was blind AND loose at once; (3) tests squarely about this work carry no keyword at all
# (`phy_metrics_deferred_chain_test` - "otherwise the chain silently degrades to the synchronous
# per-symbol path"; `resource_grid_device_view_test` - the zero-copy premise of the whole chain;
# `dft_processor_test` - the CPU twin of the metal DFT test). The project already has the right gate:
# the `phy` LABEL. Three PHY tests carry a different label, so they are topped up by name (disjoint).
FILTER_LABEL="phy"
FORMER_TOPUP="du_low_phy_pipeline_test|baseband_gateway_buffer_metal_smoke_test|pusch_processor_benchmark"
stage run "ctest -L $FILTER_LABEL (193 tests, ~25 s)"
ctest --test-dir build -L "$FILTER_LABEL" >"$T/ctest" 2>&1
cl=$(grep -E "tests passed" "$T/ctest" | tail -1)
check "ctest -L phy (the label gate): 193 runnable at the merged HEAD (was 179 before the origin/main merge)" "193" \
      "$([ "$cl" = "100% tests passed out of 193" ] && echo PASS || echo "$([ -z "$cl" ] && echo RED || echo FAIL)")" \
      "${cl:-<unreadable>}  (replaces the name-regex gate: 157 selected, 19 of them substring noise)"
# The three PHY tests the label used to miss are inside it now (they were a manual top-up); if one of
# them loses its label again, this reads RED and the gate silently shrinks - which is the whole lesson.
n=$(ctest --test-dir build -N -L "$FILTER_LABEL" -R "$FORMER_TOPUP" 2>/dev/null | grep -oE "^Total Tests: [0-9]+" | grep -oE "[0-9]+")
check "the 3 formerly-mislabelled PHY tests are inside -L phy" "3" \
      "$([ "$n" = "3" ] && echo PASS || echo FAIL)" \
      "du_low_phy_pipeline_test / baseband_gateway_buffer_metal_smoke_test / pusch_processor_benchmark: $n of 3"

./build/tests/unittests/support/executors/ul_pipeline_probe_test >"$T/probe" 2>&1
pl=$(grep -E "PASSED" "$T/probe" | tail -1)
check "ul_pipeline_probe_test: 6/6" "6" \
      "$(echo "$pl" | grep -q "6 tests" && echo PASS || echo "$([ -z "$pl" ] && echo RED || echo FAIL)")" "${pl:-<unreadable>}"

./build/tests/unittests/phy/lower/lower_phy_test >"$T/lp" 2>&1
ll=$(grep -E "PASSED" "$T/lp" | tail -1)
check "lower_phy_test: 528/528" "528" \
      "$(echo "$ll" | grep -q "528 tests" && echo PASS || echo "$([ -z "$ll" ] && echo RED || echo FAIL)")" "${ll:-<unreadable>}"

check_rerun "port_channel_estimator_metal_mmse_unit_test (6.5 flake rule: rerun once if red)" \
            ./build/lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_unit_test

# ---------------------------------------------------------------- 5. the newest DEFAULT leg: NAMES, then numbers
NAMES="radio sample continuity|dft radio inputs|zero-copy wraps|ce device estimates|host device data crossings|cfo compensation|baseband metrics|host sample assembly"
got=0
# An ARRAY, not `for n in $(echo ... | tr '|' '\n')`: the names contain spaces and command
# substitution splits on every IFS character, so the newline trick still yields words - the check
# silently read "0 of 8" while looking like it had run (this script's own first run caught it).
IFS='|' read -r -a NAMEARR <<<"$NAMES"
LEGF=""
if [ -n "$LEG" ]; then
  LEGF="$LOGDIR/gnb_gpu_${LEG}.log.stderr"
  [ -f "$LEGF" ] || LEGF=$(ls -1t "$LOGDIR"/gnb_*"$LEG"*.log.stderr 2>/dev/null | head -1)
fi
if [ -n "${LEGF:-}" ] && [ -f "$LEGF" ]; then
  leg_commit_check "$LEG" "$LEGF" "default"
  for n in "${NAMEARR[@]}"; do
    grep -qF "]   $n:" "$LEGF" && got=$((got+1))
  done
  check "leg $LEG: the 8 contract NAMES are all present (pitfall 28: names, not the number)" "8" \
        "$([ "$got" = "8" ] && echo PASS || echo FAIL)" "found $got of 8 in $(basename "$LEGF")"

  ml=$(grep -aE "contract MET" "$LEGF" | tail -1)
  check "leg $LEG: contract MET (8 of 8) and mode=gpu" "MET (8 of 8" \
        "$(echo "$ml" | grep -q "MET (8 of 8" && grep -q "contract (mode=gpu)" "$LEGF" && echo PASS || echo "$([ -z "$ml" ] && echo RED || echo FAIL)")" \
        "${ml:-<unreadable>}"

  xl=$(grep -aE "= [0-9.]+ read\(s\)" "$LEGF" | tail -1)
  check "leg $LEG: crossings 0.00 + 0.00 per hop" "0.00 + 0.00" \
        "$(echo "$xl" | grep -q "= 0.00 read(s) + 0.00 write(s) per hop" && echo PASS || echo "$([ -z "$xl" ] && echo RED || echo FAIL)")" \
        "$(echo "${xl:-<unreadable>}" | grep -oE "over [0-9]+ device hop\(s\) = [0-9.]+ read\(s\) \+ [0-9.]+ write\(s\) per hop" || echo '<unreadable>')"

  rep=$(grep -aE "counted by the module" "$LEGF" | tail -1 | grep -oE "audited their host <-> device data touches: .*" | sed 's/ (a module NOT listed.*//')
  check "leg $LEG: the crossings line NAMES its reporters (declared scope)" "4 modules" \
        "$([ -n "$rep" ] && echo PASS || echo RED)" "${rep:-<unreadable>}"

  sl=$(grep -aE "\[ul_pipeline\] stale=" "$LEGF" | tail -1)
  check "leg $LEG: stale = 0" "stale=0" \
        "$(echo "$sl" | grep -q "stale=0" && echo PASS || echo "$([ -z "$sl" ] && echo RED || echo FAIL)")" "${sl:-<unreadable>}"
else
  check "leg ${LEG:-<none>}: readable shutdown report (DEFAULT regime)" "8 names / 8 of 8 / 0.00+0.00" RED \
        "no DEFAULT-regime leg matched '${LEG:-<none>}' in $(basename "$LOGDIR") - a stress leg cannot stand in for one (5.9.127 R4)"
fi

# ---------------------------------------------------------------- 5b. the newest STRESS leg: only what load cannot change
# A stressed leg is judged on the properties that are regime-INDEPENDENT - the contract's names,
# MET + mode=gpu, the crossing count, and gaps - because those are exactly the ones a heavy uplink
# could break (s78 broke MET and gaps=1). Its regime-DEPENDENT numbers (stale, RF failures, the RX
# pool) are REPORTED, not judged: the judgement for this regime is the pre-registered V1-V5 set
# (5.9.130 (5): span <= 2150us, starved_events 0, RF <= 10 and gaps 0, cbs/lane <= 2.00, contract
# 8/8), which is what the latency workstream is held to - and it is not met yet. That is the work,
# not a finding of this gate, so this gate must not colour it red here.
SF=""
if [ -n "$STRESSLEG" ]; then
  SF="$LOGDIR/gnb_gpu_${STRESSLEG}.log.stderr"
  [ -f "$SF" ] || SF=$(ls -1t "$LOGDIR"/gnb_*"$STRESSLEG"*.log.stderr 2>/dev/null | head -1)
fi
if [ -n "${SF:-}" ] && [ -f "$SF" ]; then
  leg_commit_check "$STRESSLEG" "$SF" "stress"
  sgot=0
  for n in "${NAMEARR[@]}"; do grep -qF "]   $n:" "$SF" && sgot=$((sgot+1)); done
  check "stress leg $STRESSLEG: the 8 contract NAMES are all present" "8" \
        "$([ "$sgot" = "8" ] && echo PASS || echo FAIL)" "found $sgot of 8 in $(basename "$SF")"

  sml=$(grep -aE "contract MET" "$SF" | tail -1)
  check "stress leg $STRESSLEG: contract MET (8 of 8) and mode=gpu" "MET (8 of 8" \
        "$(echo "$sml" | grep -q "MET (8 of 8" && grep -q "contract (mode=gpu)" "$SF" && echo PASS || echo "$([ -z "$sml" ] && echo RED || echo FAIL)")" \
        "${sml:-<unreadable>}"

  sxl=$(grep -aE "= [0-9.]+ read\(s\)" "$SF" | tail -1)
  check "stress leg $STRESSLEG: crossings 0.00 + 0.00 per hop (load must not add one)" "0.00 + 0.00" \
        "$(echo "$sxl" | grep -q "= 0.00 read(s) + 0.00 write(s) per hop" && echo PASS || echo "$([ -z "$sxl" ] && echo RED || echo FAIL)")" \
        "$(echo "${sxl:-<unreadable>}" | grep -oE "over [0-9]+ device hop\(s\) = [0-9.]+ read\(s\) \+ [0-9.]+ write\(s\) per hop" || echo '<unreadable>')"

  sgl=$(grep -aE "^\[ul_rx\] blocks=" "$SF" | tail -1)
  check "stress leg $STRESSLEG: 0 gaps (the receiver never lost the radio's stream)" "gaps=0" \
        "$(echo "$sgl" | grep -q "gaps=0 " && echo PASS || echo "$([ -z "$sgl" ] && echo RED || echo FAIL)")" \
        "$(echo "${sgl:-<unreadable>}" | grep -oE "blocks=[0-9]+ samples=[0-9]+ gaps=[0-9]+" || echo '<unreadable>')"

  ssl=$(grep -aE "\[ul_pipeline\] stale=" "$SF" | tail -1)
  spl=$(grep -aE "\[ul_rx_pool\]" "$SF" | tail -1)
  check "stress leg $STRESSLEG: regime-dependent numbers (the V1-V5 target, reported not judged)" \
        "reported; V1-V5 is the latency workstream's criterion" INFO \
        "${ssl:-stale <unreadable>}${spl:+  ||  $spl}"
elif [ -n "$STRESSLEG" ]; then
  check "stress leg $STRESSLEG: readable shutdown report" "8 names / MET / 0.00+0.00 / gaps=0" RED "no .log.stderr matched '$STRESSLEG'"
fi

stage run "the two legs' criteria and the A1-2 attribution gate"
# A1-2 (5.9.118): who submits the plain-route transforms. TWO things bind this gate, and it now knows
# both: the REGIME (C5 keeps `stale=0` only in the default regime; under load 5.9.127's R4 licenses the
# opposite) and the GEOMETRY (C4's "12 per radio frame" identity is a law of the n78 PRACH configuration
# - format B4, one occasion per frame, and PRACH actually on the Metal engine). Measured 2026-09-24 on
# the n1 default leg: the plain route reads 1 (its warm-up alone, `dft commits=1`) although the leg DID
# detect preambles, i.e. PRACH's IDFTs took the CPU fallback the Metal factory documents - so there is no
# plain-route signal to attribute, and calling that a failure would be a criterion bound to the wrong
# geometry (the third such binding this session, after regime and traffic). The gate reports "NOT
# JUDGED", and this criterion then moves to a leg that CAN judge it - the stress leg (n78), where the
# stress regime also drops C5's stale condition. Judged wherever it can be judged, never softened.
a12_run() {   # <leg> <regime> <outfile>  -> echoes the summary line
  [ -n "${1:-}" ] || return 0
  bash $W/a12_attribution_gate.sh --regime="$2" "$1" >"$3" 2>&1
  grep -E "criteria pass" "$3" | tail -1
}
a12_leg=$LEG
al=$(a12_run "$LEG" default "$T/a12")
if ! echo "${al:-}" | grep -q "5 of 5 criteria pass" && grep -q "NOT JUDGED" "$T/a12" 2>/dev/null && [ -n "${STRESSLEG:-}" ]; then
  als=$(a12_run "$STRESSLEG" stress "$T/a12s")
  if echo "${als:-}" | grep -q "5 of 5 criteria pass"; then
    al="$als  [judged on the STRESS leg: the default leg cannot judge C4 - its PRACH never reaches the Metal DFT engine, so the plain route carries no signal there]"
    a12_leg=$STRESSLEG
  fi
fi
check "A1-2 attribution gate (5.9.118): 5 of 5 judged" "5 of 5" \
      "$(echo "${al:-}" | grep -q "5 of 5 criteria pass" && echo PASS || echo "$([ -z "${al:-}" ] && echo RED || echo FAIL)")" \
      "leg ${a12_leg:-<none>}: ${al:-<unreadable>}"

# ---------------------------------------------------------------- 6. the other toolchain (opt-in, INFO)
# macOS/Clang and Linux/GCC do NOT share their -Wshadow coverage. Measured 2026-09-24 with the same
# -Wshadow -Werror: a lambda parameter shadowing an enclosing local is an ERROR under g++ and SILENT
# under clang++, and that difference is what broke the Ubuntu build while this tree was green (5.9.122).
# So: a green run here is evidence about Clang, not about Ubuntu. Set MILESTONE_AUDIT_REMOTE=<user@host>
# (expecting the tree at ~/work/ocudu) and this prints what the other toolchain did. INFORMATION, not a
# criterion - that machine is not always reachable, and a gate that needs it would be a gate nobody runs.
if [ -n "${MILESTONE_AUDIT_REMOTE:-}" ]; then
  if rout=$(ssh -o BatchMode=yes -o ConnectTimeout=8 "$MILESTONE_AUDIT_REMOTE" \
             'cd ~/work/ocudu && printf "HEAD %s | " "$(git rev-parse --short=10 HEAD)" && cmake --build build -j$(nproc) 2>&1 | grep -cE "error:|Error [0-9]" | sed "s/^/errors=/"' 2>&1); then
    check "[INFO] remote build ($MILESTONE_AUDIT_REMOTE)" "errors=0" INFO "$(echo "$rout" | tr '\n' ' ')"
  else
    check "[INFO] remote build ($MILESTONE_AUDIT_REMOTE)" "errors=0" INFO "unreachable/failed: $(echo "$rout" | tail -1)"
  fi
else
  check "[INFO] remote build (other toolchain)" "errors=0" INFO \
        "not checked: set MILESTONE_AUDIT_REMOTE=jwang@192.168.31.211 to include it - Clang-green is not GCC-green"
fi

# ---------------------------------------------------------------- report
echo "milestone audit   (tree $(git rev-parse --short=10 HEAD))"
echo "  default-regime leg: ${LEG:-<none>}  <- contract, stale=0, A1-2 5/5  (5.9.118 (3) C5)"
echo "  stress-regime leg : ${STRESSLEG:-<none>}  <- contract, crossings, gaps; V1-V5 reported (5.9.130 (5))"
echo
info=0; red=0; fail=0
for r in "${rows[@]}"; do
  IFS='|' read -r name exp verdict detail <<<"$r"
  printf '  [%-16s] %s\n' "$verdict" "$name"
  printf '                     expected: %s\n' "$exp"
  printf '                     read    : %s\n' "$detail"
  case "$verdict" in INFO) info=$((info+1));; RED) red=$((red+1));; FAIL) fail=$((fail+1));; esac
done
n=${#rows[@]}
echo
echo "  $((n-info-red-fail)) PASS, $fail FAIL, $red RED(cannot read), $info INFO  (of $n)"
if [ "$fail" = 0 ] && [ "$red" = 0 ]; then
  echo "  offline acceptance: GREEN"
else
  echo "  offline acceptance: NOT GREEN - every FAIL/RED above must be explained before a milestone is declared"
fi
cat <<'AIR'

  NOT COVERED HERE (they need root + B210 + a UE; run them as legs, not offline):
    * the air leg itself            : bash wip/run_leg.sh gpu <label>   (stop with ONE Ctrl-C)
    * the 9 pre-registered criteria : bash wip/leg_gate.sh --slot-ms=0.5 <label>
    * lane cost / headroom          : bash wip/ul_load.sh --slot-us=500 <label>
    * the fenced fix's air price    : bash wip/corr_fenced_ab.sh
    * K1's barrier slope            : bash wip/k1_barrier_slope.sh
  Read the report only if wip/run_leg.sh did NOT print "THIS LEG HAS NO REPORT".
AIR
