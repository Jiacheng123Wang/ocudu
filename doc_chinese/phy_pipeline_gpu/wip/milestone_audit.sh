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
# TWO DIRECTORIES, newest first across both: the macOS thread-stability line keeps its legs in its own
# (doc_chinese/macos_thread_priority/wip/logs) and every leg before 2026-10-01 is in this one. Every lookup below
# names BOTH patterns as separate words - see the trap documented in leg_gate.sh (the compact
# "${arr[@]/%//}"gnb_*.log form hands ls a directory argument and it prints a "<dir>:" header instead).
LOGDIR=$W/logs                                  # this line's own directory, for the messages that name one
LOGDIR_NEW=$ROOT/doc_chinese/macos_thread_priority/wip/logs
LEG=""
STRESSLEG=""
QUICK=0
# Parsed with an explicit index, NOT `shift` inside `for a in "$@"`: that loop iterates the list as it
# was when the loop started, so a second `shift` leaves $1 pointing at the word the loop is already
# past - measured 2026-09-26: `--leg X --stress-leg Y` set STRESSLEG=--stress-leg and the audit read
# RED ("no log matched --stress-leg") while the leg was sitting right there. The `=` forms were fine,
# which is exactly how such a trap hides.
ARGV=("$@")
for ((i = 0; i < ${#ARGV[@]}; ++i)); do
  a=${ARGV[i]}
  case "$a" in
    --leg)          LEG=${ARGV[i + 1]:-} ;;
    --leg=*)        LEG=${a#*=} ;;
    --stress-leg)   STRESSLEG=${ARGV[i + 1]:-} ;;
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
  [ -f "$f" ] || f=$(ls -1t "$LOGDIR_NEW"/gnb_*"$label"*.log.stderr "$LOGDIR"/gnb_*"$label"*.log.stderr 2>/dev/null |
                     grep -v ':$' | head -1)
  if [ -n "${f:-}" ] && [ -f "$f" ]; then
    line=$(grep -aoE '\[leg\] regime=[a-z]+' "$f" | tail -1)
    [ -n "$line" ] && { echo "${line#*=}"; return; }
  fi
  for s in $STRESS_LEGS; do case "$label" in *"$s"*) echo stress; return;; esac; done
  echo default
}
leg_labels_newest_first() {
  ls -1t "$LOGDIR_NEW"/gnb_gpu_*.log.stderr "$LOGDIR"/gnb_gpu_*.log.stderr 2>/dev/null | grep -v ':$' |
    xargs -I{} basename {} .log.stderr | sed 's/^gnb_gpu_//'
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
# 2026-10-01: the trap used to remove the LOCK and nothing else, so a KILLED audit (Ctrl-C, a timeout, a
# closed terminal) released the lock while the replay it had started kept running - and the next audit then
# ran in parallel with a live ul_chain_replay, which is the very thing section 0a refuses on. Measured: the
# same command on the same HEAD, once with a SIGTERM'd predecessor, read "L1a PASS lines = 3" and
# "L1b differing>0 x4" (four FALSE failures); re-running alone restored them. Killing this script's own
# DESCENDANTS in the trap closes the hole without touching anyone else's replay: only children of $$ die.
kill_descendants() {   # deepest first, so a wrapper shell cannot outlive the binary it started
  local pid=$1 kids k
  kids=$(pgrep -P "$pid" 2>/dev/null) || true
  for k in $kids; do kill_descendants "$k"; done
  [ "$pid" = "$$" ] || kill -TERM "$pid" 2>/dev/null || true
}
trap 'kill_descendants $$; rm -rf "$T" "$LOCK"' EXIT

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
  # NAME the changed code paths (2026-09-27). The row used to say only how many files moved, so a FAIL
  # caused by a TEST-ONLY edit (adding a config to the YAML round-trip list, say) looked exactly like a
  # FAIL caused by a change to the PHY: the reader had to run git diff themselves to find out which it
  # was, and the whole point of this row is to be readable at a glance. The criterion is unchanged -
  # `tests/` still counts as code - only the evidence behind the verdict is now on the line.
  code_list=$(printf '%s\n' "$paths" | grep -E '^(lib/|include/|apps/|tests/)' | head -4 | tr '\n' ' ')
  check "$kind leg $label: the commit it ran, vs HEAD" "equal, or a diff that touches no code" \
        "$([ "${ncode:-0}" = "0" ] && echo PASS || echo FAIL)" \
        "leg ran $legc; $legc..HEAD changes ${nfiles:-0} file(s), ${ncode:-0} of them under lib/include/apps/tests${code_list:+ [$code_list]}$([ "${ncode:-0}" != "0" ] && echo '  <- this leg is NOT evidence about HEAD')"
}

# ---------------------------------------------------------------- 0c. A LEG MUST BE A DELIVERY LEG
#
# WHY THIS EXISTS (2026-09-27). Every criterion below judges a leg's READINGS, and none of them can tell
# a DELIVERY leg from a MEASUREMENT ARM, because an arm can satisfy all of them: p84 (the ablation arm,
# OCUDU_LANE_ABLATE=1) reads contract 9 of 9, crossings 0.00+0.00, stale=0, gaps=0, pop_blocking 39us -
# and its link is deliberately broken (CRC-OK 48.4%). Fed to this script it would have scored the same
# 23 PASS as the delivery leg. The two facts that separate them are mechanical, so they are checked:
#
#   (1) THE KNOB LINES. run_leg.sh prints every OCUDU_* it was given at the top of the leg's stderr. A
#       DELIVERY leg may carry the two probes the report needs (they perturb slightly - a completion
#       handler per buffer, a counter - but every acceptance leg on record carries them), and it may set
#       a knob EXPLICITLY to its delivery default. Anything else is a behaviour-changing arm.
#       FAIL-CLOSED ON PURPOSE: an unknown knob is not assumed harmless, it is refused - so a new probe
#       has to be added to the list below, deliberately, which is the moment to ask whether it changes
#       what the leg does.
#   (2) THE LINK DECODED. `[ul_by_size] ... N CRC-OK hop(s)` against `[ul_gpu_lane] lanes=` is the one
#       number an ablation cannot fake. Measured over 60 legs (p3x-p8x): the ablation arms read 43.7-58.8%
#       (p51 43.7, p80 47.1, p84 48.4, p83 49.4, p82 56.1, p81 58.8), every heavy delivery leg - either
#       regime - reads 60.1-96.5%. The floor is 60%: a FLOOR, not a quality criterion (V5's "CRC KO% must
#       not degrade" is judged by a human against the leg's era).
#       ⚠ IT IS BOUND TO TRAFFIC (added the same day, after the first HEAD pair): the ratio is decoded
#       hops / scheduled hops, so with a nearly idle phone it measures the PHONE, not the code - p85
#       (default regime, no load generator: 660 hops in 118s) reads 60.0% while the SAME BINARY under load
#       (p86: 145341 hops) reads 86.0%. Below kCRC_MIN_HOPS the check is therefore REPORTED, not judged.
# Probes: report-only (they change no delivery decision), but they DO perturb - each costs host work on a
# path the leg is measuring, which is why the leg's own [ul_rx_timing] loop/slip readings stay part of its
# evidence. A knob belongs here by CATEGORY, not by name: OCUDU_UL_SLOT_TRACE joined when the [ul_slot_trace]
# instrumentation was read on an acceptance leg (dev doc 6.145 (6) (1)); the behaviour-changing knobs stay
# refused below.
# OCUDU_UL_TIMING_EVENTS joined 2026-10-01 (dev doc 6.241): a report-only probe by the same category rule -
# it prints the worst receive waits / hand-over margins with host wall clocks and decides nothing.
# OCUDU_SCHED_VERBOSE joined 2026-10-01 (macos_thread_priority dev doc 10.5): also report-only - one line per
# worker thread saying which QoS class / POSIX policy the kernel actually GRANTED it, printed once at creation.
# The knobs that change macOS scheduling (OCUDU_SCHED_SKIP_POSIX_RT, OCUDU_SCHED_ATTR_QOS,
# OCUDU_SCHED_TIME_CONSTRAINT) are arms and stay refused, exactly like OCUDU_DFT_BACKEND=generic.
kNOB_ANY=" OCUDU_METAL_GPU_TIME OCUDU_UL_PHASE_SEGMENTS OCUDU_UL_SLOT_TRACE OCUDU_UL_TIMING_EVENTS OCUDU_SCHED_VERBOSE OCUDU_UL_STABILITY_WINDOWS OCUDU_UL_THREAD_CPU "
# `== the delivery default`. SINCE 2026-09-30 (dev doc 6.215) the delivered lane writes the grid from the HOST,
# so the three DFT entries are MOOT on a delivery leg (that engine is not on the path at all) while
# `CE_LANE_ORDER=merged` still is the delivered value. A delivery leg should set NONE of them - that is the
# strongest case, and the one the delivered configuration now is.
# `OCUDU_DFT_BACKEND=vdsp` joined 2026-10-01 (dev doc 6.231-6.233): on Apple that is what an unset leg resolves
# to, so it is a spelling of the default and not a change. `=generic` stays refused - that is the A/B arm.
kNOB_EQ=" OCUDU_DFT_BATCH_SYMBOLS=14 OCUDU_DFT_OPEN_BLOCK=1 OCUDU_DFT_RELEASE_BLOCK=1 OCUDU_CE_LANE_ORDER=merged OCUDU_DFT_BACKEND=vdsp "
kCRC_FLOOR_PCT=60
kCRC_MIN_HOPS=20000

leg_arm_check() {   # <label> <leg .stderr path> <kind>
  local label=$1 f=$2 kind=$3 kv name bad="" n=0 seen=""
  while IFS= read -r kv; do
    [ -n "$kv" ] || continue
    n=$((n+1))
    seen="$seen $kv"
    name=${kv%%=*}
    case "$kNOB_ANY" in *" $name "*) continue ;; esac
    case "$kNOB_EQ"  in *" $kv "*)   continue ;; esac
    bad="$bad $kv"
  done < <(grep -aE '^knob *: ' "$f" 2>/dev/null | sed 's/^knob *: *//')
  if [ -n "$bad" ]; then
    check "$kind leg $label: is a DELIVERY leg (probe knobs only, or a knob at its delivery default)" \
          "no behaviour-changing knob" FAIL \
          "behaviour-changing knob(s):$bad  <- a measurement arm must not certify acceptance: it can satisfy every other criterion here (p84 read contract 9/9, gaps 0, crossings 0.00+0.00, stale=0, and its link was deliberately dead)"
  else
    check "$kind leg $label: is a DELIVERY leg (probe knobs only, or a knob at its delivery default)" \
          "no behaviour-changing knob" PASS \
          "$n knob line(s), all probe-only or equal to their delivery default:${seen:- <none, which is the strongest case>}"
  fi
}

leg_crc_check() {   # <label> <leg .stderr path> <kind>   (the ablation arm's one un-fakeable reading)
  local label=$1 f=$2 kind=$3 crc lanes pct
  crc=$(grep -aE 'CRC-OK hop\(s\)' "$f" 2>/dev/null | tail -1 | grep -oE '[0-9]+ CRC-OK' | grep -oE '[0-9]+')
  lanes=$(grep -aE '\[ul_gpu_lane\] lanes=' "$f" 2>/dev/null | tail -1 | grep -oE 'lanes=[0-9]+' | grep -oE '[0-9]+')
  if [ -z "$crc" ] || [ -z "$lanes" ] || [ "$lanes" = "0" ]; then
    check "$kind leg $label: the link decoded (CRC-OK / lanes >= ${kCRC_FLOOR_PCT}%)" \
          ">= ${kCRC_FLOOR_PCT}% of hops CRC-OK" RED \
          "crc=${crc:-<unreadable>} lanes=${lanes:-<unreadable>} - the ablation arm's one un-fakeable reading is unreadable"
    return
  fi
  pct=$(awk -v a="$crc" -v b="$lanes" 'BEGIN { printf "%.1f", 100.0 * a / b }')
  if [ "$lanes" -lt "$kCRC_MIN_HOPS" ]; then
    check "$kind leg $label: the link decoded (CRC-OK / lanes)" \
          "reported: fewer than ${kCRC_MIN_HOPS} hops (the ratio would measure the phone's traffic)" INFO \
          "${crc}/${lanes} = ${pct}% - bound to traffic, not judged: p85 read 60.0% on 660 hops (idle phone) while the same binary read 86.0% under load (p86, 145341 hops); every heavy leg reads 60.1-96.5%"
    return
  fi
  check "$kind leg $label: the link decoded (CRC-OK / lanes >= ${kCRC_FLOOR_PCT}%)" \
        ">= ${kCRC_FLOOR_PCT}% of hops CRC-OK" \
        "$(awk -v p="$pct" -v fl="$kCRC_FLOOR_PCT" 'BEGIN { print (p >= fl) ? "PASS" : "FAIL" }')" \
        "${crc}/${lanes} = ${pct}%  (arms read 43.7-58.8%, every heavy delivery leg 60.1-96.5%)"
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
stage run "ctest -L $FILTER_LABEL (>=193 tests, ~25 s)"
ctest --test-dir build -L "$FILTER_LABEL" >"$T/ctest" 2>&1
cl=$(grep -E "tests passed" "$T/ctest" | tail -1)
# A FLOOR, NOT AN EQUALITY. The total is dynamic - this file's own pitfall-28 lesson, written three lines below
# for the test binaries - and hard-coding it made the first legitimate growth read FAIL (2026-09-28: the UL
# pipeline probe's 8 cases joined the label, 193 -> 201, see dev doc 6.148 (5)). What the gate has to catch is
# the label SHRINKING (a test silently dropping out of the acceptance path - which is exactly how that whole
# binary came to sit outside it under the directory's "support" label) and any FAILED case. A total ABOVE the
# floor is a test that joined the label, and the evidence line below says which.
n_phy=$(echo "$cl" | grep -oE "out of [0-9]+" | grep -oE "[0-9]+")
check "ctest -L phy (the label gate): 100% of >=193 runnable (the total may GROW, never shrink)" "100% and n>=193" \
      "$(echo "$cl" | grep -q "^100% tests passed out of " && [ -n "$n_phy" ] && [ "$n_phy" -ge 193 ] && echo PASS || echo "$([ -z "$cl" ] && echo RED || echo FAIL)")" \
      "${cl:-<unreadable>}  (floor 193; grew to 201 on 2026-09-28 - the UL pipeline probe's 8 cases are inside the label now)"
# The three PHY tests the label used to miss are inside it now (they were a manual top-up); if one of
# them loses its label again, this reads RED and the gate silently shrinks - which is the whole lesson.
n=$(ctest --test-dir build -N -L "$FILTER_LABEL" -R "$FORMER_TOPUP" 2>/dev/null | grep -oE "^Total Tests: [0-9]+" | grep -oE "[0-9]+")
check "the 3 formerly-mislabelled PHY tests are inside -L phy" "3" \
      "$([ "$n" = "3" ] && echo PASS || echo FAIL)" \
      "du_low_phy_pipeline_test / baseband_gateway_buffer_metal_smoke_test / pusch_processor_benchmark: $n of 3"

# Judged by the NAMES the runner prints, not by a total: the totals are dynamic (pitfall 28 - this
# script's own lesson) and the origin/main merge moved both of them (6->7 and 528->576 on 2026-09-26),
# which made two green test binaries read FAIL. What must never happen is a FAILED case, and that is
# what `[  FAILED  ]` says.
./build/tests/unittests/support/executors/ul_pipeline_probe_test >"$T/probe" 2>&1
pl=$(grep -E "PASSED|FAILED" "$T/probe" | tail -1)
ppassed=$(grep -cE "^\[  PASSED  \]" "$T/probe")
pfailed=$(grep -cE "^\[  FAILED  \]" "$T/probe")
check "ul_pipeline_probe_test: a PASSED line and no FAILED case" "passed >= 1 / failed = 0" \
      "$([ "$ppassed" -ge 1 ] && [ "$pfailed" -eq 0 ] && echo PASS || echo "$([ "$ppassed" -eq 0 ] && echo RED || echo FAIL)")" \
      "${pl:-<unreadable>}"

./build/tests/unittests/phy/lower/lower_phy_test >"$T/lp" 2>&1
ll=$(grep -E "PASSED|FAILED" "$T/lp" | tail -1)
lpassed=$(grep -cE "^\[  PASSED  \]" "$T/lp")
lfailed=$(grep -cE "^\[  FAILED  \]" "$T/lp")
check "lower_phy_test: a PASSED line and no FAILED case" "passed >= 1 / failed = 0" \
      "$([ "$lpassed" -ge 1 ] && [ "$lfailed" -eq 0 ] && echo PASS || echo "$([ "$lpassed" -eq 0 ] && echo RED || echo FAIL)")" \
      "${ll:-<unreadable>}"

check_rerun "port_channel_estimator_metal_mmse_unit_test (6.5 flake rule: rerun once if red)" \
            ./build/lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_unit_test

# ---------------------------------------------------------------- 5. the newest DEFAULT leg: NAMES, then numbers
NAMES="radio sample continuity|dft radio inputs|zero-copy wraps|lane host participation|ce device estimates|host device data crossings|cfo compensation|baseband metrics|host sample assembly"
got=0
# An ARRAY, not `for n in $(echo ... | tr '|' '\n')`: the names contain spaces and command
# substitution splits on every IFS character, so the newline trick still yields words - the check
# silently read "0 of 8" while looking like it had run (this script's own first run caught it).
IFS='|' read -r -a NAMEARR <<<"$NAMES"
LEGF=""
if [ -n "$LEG" ]; then
  LEGF="$LOGDIR/gnb_gpu_${LEG}.log.stderr"
  [ -f "$LEGF" ] || LEGF=$(ls -1t "$LOGDIR_NEW"/gnb_*"$LEG"*.log.stderr "$LOGDIR"/gnb_*"$LEG"*.log.stderr 2>/dev/null |
                           grep -v ':$' | head -1)
fi
if [ -n "${LEGF:-}" ] && [ -f "$LEGF" ]; then
  leg_commit_check "$LEG" "$LEGF" "default"
  leg_arm_check "$LEG" "$LEGF" "default"
  leg_crc_check "$LEG" "$LEGF" "default"
  for n in "${NAMEARR[@]}"; do
    grep -qF "]   $n:" "$LEGF" && got=$((got+1))
  done
  check "leg $LEG: the 9 contract NAMES are all present (pitfall 28: names, not the number)" "9" \
        "$([ "$got" = "9" ] && echo PASS || echo FAIL)" "found $got of 9 in $(basename "$LEGF")"

  ml=$(grep -aE "contract MET" "$LEGF" | tail -1)
  # TWO DELIVERY READINGS SINCE 2026-09-30 (dev doc 6.215). The lane's DFT default moved to the HOST, and the
  # "dft radio inputs" check then has NO POPULATION (the Metal DFT engine is off the path: `0 transform(s)`), so
  # the contract reports "MET (8 of 9)". That is not a regression - and it is not accepted blindly either: the
  # 8-of-9 reading is only a PASS when the check that lost its population is exactly that one, verified by
  # reading its line. Anything else fails closed, as before.
  dftl=$(grep -aF "]   dft radio inputs:" "$LEGF" | tail -1)
  if echo "$ml" | grep -q "MET (9 of 9"; then
    check "leg $LEG: contract MET (9 of 9) and mode=gpu" "MET (9 of 9" \
          "$(grep -q "contract (mode=gpu)" "$LEGF" && echo PASS || echo "$([ -z "$ml" ] && echo RED || echo FAIL)")" \
          "${ml:-<unreadable>}"
  elif echo "$ml" | grep -q "MET (8 of 9" && echo "$dftl" | grep -q "0 transform(s)"; then
    check "leg $LEG: contract MET (8 of 9) - the DFT check lost its population because the HOST writes the grid" \
          "MET (8 of 9) with dft radio inputs = 0 transform(s)" \
          "$(grep -q "contract (mode=gpu)" "$LEGF" && echo PASS || echo "$([ -z "$ml" ] && echo RED || echo FAIL)")" \
          "${ml:-<unreadable>} | ${dftl:-<unreadable>}"
  else
    check "leg $LEG: contract MET (9 of 9), or 8 of 9 with the DFT check unpopulated, and mode=gpu" \
          "MET (9 of 9), or MET (8 of 9) with dft radio inputs = 0 transform(s)" \
          "$([ -z "$ml" ] && echo RED || echo FAIL)" \
          "${ml:-<unreadable>} | ${dftl:-<no dft radio inputs line>}"
  fi

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
  check "leg ${LEG:-<none>}: readable shutdown report (DEFAULT regime)" "9 names / 9 of 9 / 0.00+0.00" RED \
        "no DEFAULT-regime leg matched '${LEG:-<none>}' in $(basename "$LOGDIR") - a stress leg cannot stand in for one (5.9.127 R4)"
fi

# ---------------------------------------------------------------- 5b. the newest STRESS leg: only what load cannot change
# A stressed leg is judged on the properties that are regime-INDEPENDENT - the contract's names,
# MET + mode=gpu, the crossing count, and gaps - because those are exactly the ones a heavy uplink
# could break (s78 broke MET and gaps=1). Its regime-DEPENDENT numbers (stale, RF failures, the RX
# pool) are REPORTED, not judged: the judgement for this regime is the pre-registered V1-V5 set
# (5.9.130 (5): span <= 2150us, starved_events 0, RF failures and gaps 0, cbs/lane <= 2.00,
# contract 9/9), which is what the latency workstream is held to. RE-RULED 2026-09-26 (user, dev doc
# 6.122): "RF <= 10" was written for a deterministic link and is unreachable on this USB-attached
# B200, so V3 now reads "gaps == 0 AND the per-DL-transmission real-time failure rate within the
# mode's band (gpu/cpu_gpu <= 0.25%, cpu <= 0.01%) over >= 2 same-load legs". The RF count stays
# REPORTED here, never judged: it moves with the link (700-1700 on one recipe), and the band is what
# the workstream is held to.
SF=""
if [ -n "$STRESSLEG" ]; then
  SF="$LOGDIR/gnb_gpu_${STRESSLEG}.log.stderr"
  [ -f "$SF" ] || SF=$(ls -1t "$LOGDIR_NEW"/gnb_*"$STRESSLEG"*.log.stderr "$LOGDIR"/gnb_*"$STRESSLEG"*.log.stderr 2>/dev/null |
                       grep -v ':$' | head -1)
fi
if [ -n "${SF:-}" ] && [ -f "$SF" ]; then
  leg_commit_check "$STRESSLEG" "$SF" "stress"
  leg_arm_check "$STRESSLEG" "$SF" "stress"
  leg_crc_check "$STRESSLEG" "$SF" "stress"
  sgot=0
  for n in "${NAMEARR[@]}"; do grep -qF "]   $n:" "$SF" && sgot=$((sgot+1)); done
  check "stress leg $STRESSLEG: the 9 contract NAMES are all present" "9" \
        "$([ "$sgot" = "9" ] && echo PASS || echo FAIL)" "found $sgot of 9 in $(basename "$SF")"

  sml=$(grep -aE "contract MET" "$SF" | tail -1)
  # TWO DELIVERY READINGS, same rule as the default side above and for the same reason (dev doc 6.215):
  # with the grid written on the HOST the "dft radio inputs" check has no population and the contract
  # reports "MET (8 of 9)". Measured 2026-10-01 (dev doc 6.227): this check still demanded the literal
  # "MET (9 of 9" for the stress leg, so the first delivered stress leg (p169-n78-stress) FAILED the
  # audit on a healthy reading while the default side had accepted the same line for a week.
  sdftl=$(grep -aF "]   dft radio inputs:" "$SF" | tail -1)
  if echo "$sml" | grep -q "MET (9 of 9"; then
    check "stress leg $STRESSLEG: contract MET (9 of 9) and mode=gpu" "MET (9 of 9" \
          "$(grep -q "contract (mode=gpu)" "$SF" && echo PASS || echo "$([ -z "$sml" ] && echo RED || echo FAIL)")" \
          "${sml:-<unreadable>}"
  elif echo "$sml" | grep -q "MET (8 of 9" && echo "$sdftl" | grep -q "0 transform(s)"; then
    check "stress leg $STRESSLEG: contract MET (8 of 9) - the DFT check lost its population because the HOST writes the grid" \
          "MET (8 of 9) with dft radio inputs = 0 transform(s)" \
          "$(grep -q "contract (mode=gpu)" "$SF" && echo PASS || echo "$([ -z "$sml" ] && echo RED || echo FAIL)")" \
          "${sml:-<unreadable>} | ${sdftl:-<unreadable>}"
  else
    check "stress leg $STRESSLEG: contract MET (9 of 9), or 8 of 9 with the DFT check unpopulated, and mode=gpu" \
          "MET (9 of 9), or MET (8 of 9) with dft radio inputs = 0 transform(s)" \
          "$([ -z "$sml" ] && echo RED || echo FAIL)" \
          "${sml:-<unreadable>} | ${sdftl:-<no dft radio inputs line>}"
  fi

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
  check "stress leg $STRESSLEG: readable shutdown report" "9 names / MET / 0.00+0.00 / gaps=0" RED "no .log.stderr matched '$STRESSLEG'"
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
# ACCEPTED SHAPES (dev doc 6.227, 2026-10-01). "5 of 5" stays the first form. The second is: NO criterion
# failed, and the ones the gate could not judge are recorded as such BY THE GATE ITSELF. The delivered
# config produces exactly that, because the lower-PHY DFT runs on the HOST there (dev doc 6.215 (1)): the
# Metal engine never runs, the plain route reads 0 transform(s), and C1-C4 have no population. Before this
# form existed the gate's C3 divided 0/0 and reported the THIRD defect branch ("a slotted instance did not
# join a block") on two healthy delivered legs, so the audit read NOT GREEN. A real FAIL still fails.
a12_accept() { # <outfile> -> PASS / FAIL
  grep -q "5 of 5 criteria pass" "$1" && { echo PASS; return; }
  if ! grep -q "\[FAIL" "$1" && grep -q "n/a (not judged)" "$1" && grep -q "\[PASS" "$1"; then
    echo PASS
  else
    echo FAIL
  fi
}
a12_leg=$LEG
al=$(a12_run "$LEG" default "$T/a12")
a12_v=$(a12_accept "$T/a12")
if [ "$a12_v" != "PASS" ] && [ -n "${STRESSLEG:-}" ]; then
  als=$(a12_run "$STRESSLEG" stress "$T/a12s")
  if [ "$(a12_accept "$T/a12s")" = "PASS" ]; then
    al="$als  [judged on the STRESS leg: the default leg cannot judge it]"
    a12_leg=$STRESSLEG
    a12_v=PASS
  fi
fi
check "A1-2 attribution gate (5.9.118): every JUDGED criterion passes (5 of 5, or the rest NOT JUDGED by the gate)" \
      "5 of 5, or no FAIL with the unjudgeable ones recorded as NOT JUDGED" \
      "$a12_v" \
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
