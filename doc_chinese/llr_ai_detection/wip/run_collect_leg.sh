#!/usr/bin/env bash
# One corpus-collection leg. A COPY of doc_chinese/phy_pipeline_gpu/wip/run_leg.sh with the corpus defaults filled in.
#
# WHY A COPY AND NOT A WRAPPER (design section 19.475). The first attempt was a wrapper that ran
#
#     exec sudo -E env OCUDU_UL_DUMP=... bash .../run_leg.sh cpu <label>
#
# and it has one failure the wrapper cannot fix: `exec` replaces the wrapper's process image with sudo's, so a Ctrl-C
# reaches sudo and the gNB rather than this script's trap. The leg ended, the gNB did not, and the next run refused to
# start because of the stray -- which is exactly the failure the stray check exists to catch, arriving through the one
# path that bypasses the trap. Keeping the signal handling in the same process as the launcher is not a detail; it is
# what "one leg" means. So the runner is copied here, with the collection defaults added and nothing else removed.
#
# DIFFERENCES FROM THE ORIGINAL, all in one block below the ROOT line:
#   * the default LEG_CONFIG is the n78 20 MHz collection configuration instead of the n1 5 MHz bridge;
#   * LEG_LOGDIR, OCUDU_UL_DUMP and the four capture switches default to this workstream's corpus tree;
#   * the pipeline mode defaults to cpu, because a corpus records the same PDU and grid whatever receiver runs.
# Every other behaviour -- pre-flight build, environment checks, the stray-gNB refusal, the log pair, the .protocol.txt,
# the Ctrl-C contract -- is inherited unchanged.
#
# One on-air leg of the fused-lane work, with the PIPELINE MODE as a required argument.
#
# -------------------------------------------------------------------------------------------------------------------
# WHAT A "LEG" IS (the word this file, fly_leg.sh and leg_gate.sh are named after - defined here because the whole
# record uses it and, until 2026-10-05, nothing said what it meant).
#
# A leg is ONE ON-AIR RUN OF ONE CONFIGURATION: gnb started, the phone attached and driven through its traffic
# (iperf3 or ping, started by the operator at the cue), the run stopped by protocol (Ctrl-C, never SIGTERM), and
# one configuration - one ARM - with one log pair and one .protocol.txt. It is the atomic unit of EVIDENCE here: it
# carries a label (p297), a health verdict, a gate result, and a partner leg; and it is either QUOTABLE or VOID, in
# which case it is re-flown rather than explained.
#
# WHY "LEG". The word is inherited from full_gpu_chain/wip/run_air_leg.sh ("One on-air leg of the UL/cpu_gpu
# configuration"), which this script replaces, and it comes from FLIGHT TEST: there a leg is one continuous segment
# from take-off to landing, a sortie can carry several, and each is a complete, separately readable record. An
# over-the-air run has the same properties, and they are why this workstream's rules look the way they do:
#   * EXPENSIVE and ONE-SHOT - radio, phone, and a person on site; it cannot be replayed;
#   * decided by its ENVIRONMENT - RF conditions are the weather (leg p294 was voided by 27.6% CRC, not by code);
#   * its criteria are REGISTERED BEFORE it flies - the .protocol.txt, one variable per pair;
#   * a leg that fails its gate is WASTED, not reinterpreted.
# The name is kept because the scripts, the log names (gnb_cpu_p297-dual_1005_*), the gates and the generated knob
# inventory's "legs flown" column are all built on it; renaming would leave the record with two vocabularies while
# the past logs kept the old one. Defined in the plan document as well (macos_thread_priority plan doc 11.102).
# -------------------------------------------------------------------------------------------------------------------
#
# Why this script exists instead of reusing full_gpu_chain/wip/run_air_leg.sh: that one HARD-CODES
# --expert_phy.phy_pipeline cpu_gpu, so every leg it ever ran was the module-level-offload mode -
# which by its own definition "keeps its own host <-> device crossing at every module boundary", i.e.
# exactly the mode this work is meant to move away from. A leg that cannot select the mode cannot
# judge it. Nothing in doc_chinese/full_gpu_chain/ is modified by this file.
#
# usage: sudo -E bash wip/run_leg.sh <cpu|cpu_gpu|gpu> <leg-label> [OCUDU_*=value | --option=value ...]
#   e.g. sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu     s1-unlock
#        sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh cpu_gpu baseline
#
# Two kinds of extra argument, and they take different paths into the process (the split that a
# previous leg got wrong, silently running the default while the log looked like the knob had no
# effect):
#   * OCUDU_*=…   the fusion/tuning knobs, read from the ENVIRONMENT. Exported INSIDE the root
#                 process, because under sudo an assignment written before `bash` can be stripped
#                 by env_reset even with -E;
#   * --…         a real gNB command-line option, which has to reach argv.
# Anything else is refused loudly rather than dropped.
#
# THE LEG BUILDS ITSELF FIRST (2026-10-16). `cmake --build build -j14` runs before the binary-stamp check, so
# a stamp left behind by a commit that was never built no longer costs a manual rebuild with the radio and
# the phone already up. Two environment knobs: LEG_SKIP_BUILD=1 skips it (fly the binary that is already
# there), LEG_BUILD_JOBS=<n> changes the job count (default 14). The build runs as the INVOKING user rather
# than as root, so it cannot leave root-owned objects in build/ for the next ordinary build to trip over.
set -u
MODE=${1:?pipeline mode: cpu, cpu_gpu or gpu}
LABEL=${2:?leg label such as col_1025_ping, which is also the corpus folder name}
shift 2 || true

case "$MODE" in
  cpu|cpu_gpu|gpu) ;;
  *) echo "refusing mode '$MODE': must be one of cpu, cpu_gpu, gpu" >&2; exit 2 ;;
esac

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
# The cell config decides the GEOMETRY every number in the report is relative to: PRB count (5 MHz
# n1 = 25, 20 MHz n78 = 51) and slot period (15 kHz = 1000us, 30 kHz = 500us), hence both the lane's
# occupancy and the "余量 = 1 - residency/slot" reading (wip/ul_load.sh). It is therefore part of the
# evidence, printed below, and overridable - a heavier leg needs a wider cell, and hard-coding one
# config is how the old run_air_leg.sh could only ever express one mode.
# ---- COLLECTION DEFAULTS (this workstream only) ------------------------------------------------------------
# The label names the corpus folder, so one argument identifies the leg and the place its captures go. The dump is a
# filename PREFIX, not a directory: captures are written as <prefix>_<sfn>_<slot>_<rnti>.*, so a directory-plus-filename
# is what makes them land inside the folder. run_leg.sh creates the parent below, which matters because ul_capture checks
# the directory once and latches the answer -- a leg started with it missing captures nothing for its whole duration.
COLLECT_CORPUS=${COLLECT_CORPUS:-$ROOT/doc_chinese/llr_ai_detection/work_tmp/corpus}
# ★★★ THE LEG NAME IS ITS OWN VARIABLE, AND IT IS NOT THE LABEL VERBATIM (2026-10-31).
# The old names carried three parts that say nothing about the capture: "w2" (every leg of this workstream), "harq_"
# (every leg -- all of them exist to recover HARQ donors), and the business word ("ping"/"iperf"), which stopped being a
# reliable description once legs started mixing both. What identifies a capture is the batch, the modulation and the
# leg's own suffix: w2_harq_16QAM_ping_1030a becomes s2_16QAM_ping_1030a.
#   w2        -> s2      the second batch/session
#   harq_     -> (gone)  every leg has it
#   ping_     -> kept, but only as a hint -- a leg may carry both ping and iperf
#   1030a     -> kept
# LEG_NAME can be given explicitly; otherwise it is derived from the label so both spellings name the same capture.
LEG_NAME=${LEG_NAME:-$LABEL}
if [ "$LEG_NAME" = "$LABEL" ] && [ "${LABEL#w2_harq_}" != "$LABEL" ]; then
  LEG_NAME="s2_${LABEL#w2_harq_}"
fi
export LEG_NAME
# Say the mapping out loud: the folder that appears on disk is the leg name, not the label the caller typed, and a silent
# rename is exactly the kind of thing that makes a corpus unexplainable six weeks later.
[ "$LEG_NAME" != "$LABEL" ] && echo "leg           : $LEG_NAME (label $LABEL)" >&2
export LEG_LOGDIR=${LEG_LOGDIR:-$COLLECT_CORPUS/$LEG_NAME/logs}
export OCUDU_UL_DUMP=${OCUDU_UL_DUMP:-$COLLECT_CORPUS/$LEG_NAME/cap}
export OCUDU_UL_DUMP_COUNT=${OCUDU_UL_DUMP_COUNT:-100000}
# ★ THE TRANSPORT-BLOCK DUMP DIRECTORY, AND WHY THIS LINE EXISTS. helix's dump is armed by the PRESENCE of the variable
# (port_channel_estimator_helena_impl.cpp: dump_en = getenv(...) != nullptr), and its fopen is not checked: a directory that
# is not there means no .bits for the whole leg and no complaint about it. Nothing in this script created it, so every leg
# depended on the caller remembering two lines -- export the path and mkdir it -- and forgetting either produced a leg with
# captures and no transport blocks, which is only noticed when the recovery stage finds nothing to recover.
# It now defaults beside the captures and is created here; both can still be overridden.
# ★ AN INHERITED PATH IS NOT A CHOICE. The shell keeps OCUDU_HELENA_DUMP_DIR exported after a leg finishes, so the next leg inherits the
# PREVIOUS leg's directory and writes its transport blocks there -- measured 2026-10-29: leg w2_harq_QPSK_iperf_1029b put its bits inside
# w2_harq_mcs06_ping_1029a/tb_live because that export was still in the shell, and the new leg's own tb_live was never created. A leg's
# bits must live with the leg or the provenance is a lie.
# So the path is DERIVED from the label unless the caller asks for something else by name (LEG_TB_DIR), and an inherited variable that
# does not name this leg is reported and overridden rather than obeyed.
TB_DIR_DEFAULT=$COLLECT_CORPUS/$LEG_NAME/tb_live
if [ -n "${LEG_TB_DIR:-}" ]; then
  export OCUDU_HELENA_DUMP_DIR=$LEG_TB_DIR
elif [ -n "${OCUDU_HELENA_DUMP_DIR:-}" ] && [ "$OCUDU_HELENA_DUMP_DIR" != "$TB_DIR_DEFAULT" ]; then
  if [ "$(basename "$(dirname "$OCUDU_HELENA_DUMP_DIR")")" != "$LEG_NAME" ]; then
    echo "note          : ignoring inherited OCUDU_HELENA_DUMP_DIR=$OCUDU_HELENA_DUMP_DIR (it names another leg)" >&2
    export OCUDU_HELENA_DUMP_DIR=$TB_DIR_DEFAULT
  fi
else
  export OCUDU_HELENA_DUMP_DIR=$TB_DIR_DEFAULT
fi
mkdir -p "$OCUDU_HELENA_DUMP_DIR"
# ★ THE RV FILTER IS DELIBERATE WHEN SET, AND MUST BE SAID OUT LOUD. Unset captures every redundancy version, which is what a
# HARQ-recovery corpus wants; setting it keeps only that one, which is occasionally the point. Either way the console has to
# say which it is, because a leg that captured only rv=0 looks exactly like a leg where the retransmissions never happened.
if [ -n "${OCUDU_UL_DUMP_RV:-}" ]; then
  RV_NOTE="rv=${OCUDU_UL_DUMP_RV} ONLY (filtered on purpose)"
else
  RV_NOTE="all rv"
fi
# OFF BY DEFAULT SINCE 2026-10-27. The soft-bit dump answers a question we no longer ask during collection: it records
# the LLRs ONE arm produced at capture time, which cannot stand in for another arm and is not read by the dataset
# builder (it opens the grid, the channel estimate, the estimator sidecar, the PDU and the transsport block). It was on
# because this script turned it on, and it has been accumulating tens of gigabytes per long leg. The capability stays:
# any capture can be replayed through an arm to dump its LLRs, which is what the LLR-vs-classical comparison does.
export OCUDU_UL_DUMP_LLR=${OCUDU_UL_DUMP_LLR:-0}
export OCUDU_LA_LOG_EVERY=${OCUDU_LA_LOG_EVERY:-1}

CONFIG=${LEG_CONFIG:-$ROOT/configs/gnb_rf_b200_tdd_n78_20mhz_llr_collect_p0default.yml}
if [ ! -f "$CONFIG" ]; then
  echo "REFUSING to run: LEG_CONFIG=$CONFIG is not a file." >&2
  exit 2
fi
# THE CONFIG'S OWN ADDRESS MUST EXIST ON THIS MACHINE, OR THE LEG CANNOT START.
# Two failures, one cause, both measured: a misspelt variable (LEG_ONFIG=...) leaves this script reading its DEFAULT config, which
# names the address of whichever site the default belongs to; and a config that was right at one site is wrong at the next. Either way
# gnb starts, cannot bind its NG-U socket, and exits 1 about two seconds later with "Failed to bind UDP socket to <addr>:2152" -- a
# message that looks like a network fault and is really a stale file. Refusing here says which file and which address, which is the
# difference between a five-second fix and a lost leg.
BIND_ADDR=$(awk '/^[[:space:]]*bind_addrs:/{sub(/^[[:space:]]*bind_addrs:[[:space:]]*/, ""); gsub(/["\047]/, ""); print; exit}' "$CONFIG")
if [ -n "$BIND_ADDR" ] && [ "$BIND_ADDR" != "0.0.0.0" ]; then
  LOCAL_IPS=$(ifconfig 2>/dev/null | awk '/inet /{sub(/^addr:/, "", $2); sub(/\/.*/, "", $2); print $2}')
  if ! printf '%s\n' "$LOCAL_IPS" | grep -qx "$BIND_ADDR"; then
    echo "REFUSING to run: $CONFIG binds NG-U to $BIND_ADDR, which is not an address of this machine." >&2
    echo "  this machine has: $(printf '%s ' $LOCAL_IPS)" >&2
    echo "  LEG_CONFIG currently = ${LEG_CONFIG:-<unset: using the DEFAULT config above>}" >&2
    echo "  Fix: pass LEG_CONFIG=<config for this site>, or correct bind_addrs in $CONFIG." >&2
    exit 2
  fi
fi
# WHERE THE LEG'S THREE FILES GO. The default is this directory's logs - every leg flown before 2026-10-01 is
# there - and LEG_LOGDIR overrides it for a workstream that keeps its own: doc_chinese/macos_thread_priority/
# README.md declares that this line's legs live in doc_chinese/macos_thread_priority/wip/logs, and the three
# GATES (leg_gate.sh, ul_load.sh, milestone_audit.sh) search BOTH directories, newest first, so a leg is found
# wherever this puts it. Measured 2026-10-01: p183-n78-default flew into the historical directory while that
# README said otherwise, which is exactly the kind of "the record says one thing, the tool does another" this
# tree keeps writing post-mortems about.
LOGDIR=${LEG_LOGDIR}
mkdir -p "$LOGDIR"
# The mode is part of the file name: an A/B across modes must never be able to overwrite one arm
# with the other, which is the mistake that makes a comparison silently compare a run with itself.
LOG=$LOGDIR/gnb_${MODE}_${LABEL}_$(date +%m%d_%H%M).log

# ---- PRE-FLIGHT BUILD: a stamp mismatch must not need a manual rebuild -----------------------------
# The check below refuses a leg whose binary is not the commit under test, and it is right to: an on-air leg
# costs the operator a phone test. What it must not do is stop a leg for a reason one command fixes -- and
# that is what kept happening. Measured 2026-10-16: the operator had to rebuild by hand and re-fly, with the
# radio, the phone and the traffic generator already up, because build/hashes.h had been regenerated without
# relinking. `gen_build_info` is an ordinary custom target that rewrites build/hashes.h on every build, so a
# plain `cmake --build build` refreshes the stamp AND relinks the binary that has to carry it. Doing it here
# leaves the check below to catch only what a build cannot fix: a tree that does not compile, or a binary
# that is older than the header it was stamped from. With nothing to do it costs a few seconds.
#
# Built as the INVOKING user, never as root. This script is run through sudo, and a build run as root leaves
# root-owned objects in build/ that the next ordinary build cannot overwrite -- a permission error a long way
# from its cause. The nested sudo is non-interactive on purpose: a pre-flight step that can stop and wait for
# a password is worse than no pre-flight step, so when it cannot run as the invoking user it says so and
# leaves the decision to the stamp check.
if [ -z "${LEG_SKIP_BUILD:-}" ]; then
  CMAKE_BIN=$(command -v cmake || true)
  BUILD_JOBS=${LEG_BUILD_JOBS:-14}
  if [ -z "$CMAKE_BIN" ]; then
    echo "WARNING: cmake is not on PATH: skipping the pre-flight build; the stamp check below still applies." >&2
  else
    echo "pre-flight build: $CMAKE_BIN --build $ROOT/build -j$BUILD_JOBS   (LEG_SKIP_BUILD=1 skips it)"
    _build_t0=$(date +%s)
    _build_rc=0
    if [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != "root" ] && sudo -n -u "$SUDO_USER" true 2>/dev/null; then
      sudo -n -H -u "$SUDO_USER" "$CMAKE_BIN" --build "$ROOT/build" -j"$BUILD_JOBS" || _build_rc=$?
    elif [ -z "${SUDO_USER:-}" ] || [ "$SUDO_USER" = "root" ]; then
      "$CMAKE_BIN" --build "$ROOT/build" -j"$BUILD_JOBS" || _build_rc=$?
    else
      echo "WARNING: the pre-flight build cannot run as $SUDO_USER without a password." >&2
      echo "         Build it yourself (cmake --build build -j$BUILD_JOBS) or set LEG_SKIP_BUILD=1." >&2
    fi
    echo "pre-flight build: rc=$_build_rc after $(( $(date +%s) - _build_t0 ))s"
    if [ "$_build_rc" -ne 0 ]; then
      echo "REFUSING to run: the pre-flight build failed (rc=$_build_rc)." >&2
      echo "  A leg is evidence about a binary, and this tree does not build one." >&2
      echo "  Fix the build, or set LEG_SKIP_BUILD=1 to fly what is already in build/ (the stamp check still applies)." >&2
      exit 2
    fi
  fi
fi

# ---- the binary must be the commit under test ------------------------------------------------
# A leg is evidence about a BINARY, and the binary carries the commit that was HEAD when it was
# built (build/hashes.h). If that is not the commit being tested, the leg is evidence about the
# wrong code - and the report prints the stamp, so the mistake is visible only to someone who
# compares it against HEAD, after the leg has been run and the phone tested.
#
# This has already happened once: the TAIL_DEV flip was edited, built and then committed, so the
# binary was the flipped code carrying the PREVIOUS commit's stamp (the stamp is taken at build
# time, and the commit did not exist yet). The leg was valid; the banner was not, and only a manual
# read of the report caught it.
#
# Fixing it is one command, so refuse rather than warn: an OTA leg costs the operator a phone test.
STAMP=$(grep -oE '^[[:space:]]*#[[:space:]]*define[[:space:]]+OCUDU_[A-Z_]*COMMIT[[:space:]]+"[0-9a-f]+"' "$ROOT/build/hashes.h" 2>/dev/null | grep -oE '[0-9a-f]{7,40}' | head -1)
if [ -z "$STAMP" ]; then
  STAMP=$(grep -oE '[0-9a-f]{10}' "$ROOT/build/hashes.h" 2>/dev/null | head -1)
fi
HEAD_SHORT=$(git -C "$ROOT" rev-parse --short=10 HEAD 2>/dev/null)
if [ -z "$STAMP" ]; then
  echo "WARNING: cannot read a commit stamp from build/hashes.h - cannot tell which code this binary is." >&2
  echo "         the report's 'commit' line will be the only evidence." >&2
elif [ -n "$HEAD_SHORT" ] && [ "$STAMP" != "$HEAD_SHORT" ]; then
  echo "REFUSING to run: the binary is stamped $STAMP but HEAD is $HEAD_SHORT." >&2
  echo "  A leg run now would be evidence about $STAMP, not about the commit you are testing." >&2
  echo "  The pre-flight build runs first, so this means the header is not being regenerated by it" >&2
  echo "  (LEG_SKIP_BUILD was set, or build/hashes.h is stale in a way a build does not touch)." >&2
  echo "  Fix (then re-run):  cmake --build build --target ocudu_versioning && cmake --build build --target gnb" >&2
  exit 2
fi
# ---- and the stamp must actually BE IN the binary ---------------------------------------------------------
# Why this exists (2026-09-25, leg `p29-n78-dispatch`): `cmake --build build --target ocudu_versioning`
# REGENERATES build/hashes.h without relinking anything, so refreshing the stamp after a code change leaves a
# binary that CLAIMS the new commit while running the old code - and the check above passes, because it
# compares the stamp with HEAD, not the binary with the stamp. That leg's new reading was simply absent, and
# the operator paid for an OTA run to find out (dev doc 6.44 (5)). The stamp string is compiled into the
# binary (lib/support/versioning), so the honest check is content, not mtime: the binary must carry it.
if ! grep -aq "$STAMP" "$ROOT/build/apps/gnb/gnb" 2>/dev/null; then
  echo "REFUSING to run: build/apps/gnb/gnb does not carry the stamp it claims ($STAMP)." >&2
  echo "  build/hashes.h was regenerated without relinking the binary, so the code is older than the stamp." >&2
  echo "  Fix (then re-run):  cmake --build build --target ocudu_versioning && cmake --build build --target gnb" >&2
  exit 2
fi

# ---- and the code must be COMMITTED, or the stamp is only half the truth --------------------------------
# The stamp is generated from HEAD at BUILD time, so uncommitted source changes ARE in the binary and are NOT in
# the stamp: such a leg is evidence about "HEAD plus a diff nobody can see". Measured 2026-10-16: a leg was flown
# with a shutdown fix that had been built but not yet committed (18:03:55 link, commit at 18:04:46), so its log
# says d8354ac8d1 while the binary was fd0b7f82c7's code - and nothing in the report could have told them apart.
#
# The check is limited to the paths that become code: documenting a leg, or writing its recipes, does not change
# the binary and must not block flying the next one. LEG_ALLOW_DIRTY=1 bypasses it, and then the fingerprint of
# the diff is written next to the leg's files so the run stays attributable to something.
DIRTY_PATHS="lib apps include tests CMakeLists.txt"
DIRTY=$(git -C "$ROOT" status --porcelain --untracked-files=no -- $DIRTY_PATHS 2>/dev/null)
DIRTY_FP=$(git -C "$ROOT" diff -- $DIRTY_PATHS 2>/dev/null | git hash-object --stdin 2>/dev/null | cut -c1-12)
if [ -n "$DIRTY" ] && [ "${LEG_ALLOW_DIRTY:-0}" != "1" ]; then
  echo "REFUSING to run: the source tree has uncommitted changes, so this binary carries code that no commit" >&2
  echo "  describes. A leg is evidence about a BINARY; commit the changes first (or fly with LEG_ALLOW_DIRTY=1)." >&2
  echo "$DIRTY" | sed 's/^/    /' >&2
  [ -n "$DIRTY_FP" ] && echo "  the uncommitted diff hashes to: $DIRTY_FP" >&2
  exit 2
fi
if [ -n "$DIRTY" ]; then
  echo "WARNING: flying with uncommitted source changes (LEG_ALLOW_DIRTY=1); diff fingerprint: $DIRTY_FP"
  echo "dirty-code fingerprint: $DIRTY_FP" > "$LOG.dirtycode" 2>/dev/null || true
fi

mkdir -p "$LOGDIR"     # --log.filename never creates it, and a missing one fails silently

# ---- THE CAPTURE DIRECTORY MUST EXIST BEFORE THE FIRST RECEPTION -------------------------------------
# `fopen` does not create directories, and ul_capture checks the directory ONCE, before its first capture,
# then latches the answer: a leg started with the directory missing drops every reception it makes and
# never recovers, however long it runs or however good the link becomes. Measured 2026-10-16: a corpus
# directory holding a bad window was deleted just before a re-fly, so the new leg spent its whole settling
# period writing nothing -- the counters and the rate looked normal, and only the leg's .stderr file said
# "NOT CAPTURING". Creating the directory here costs nothing and removes the failure mode.
if [ -n "${OCUDU_UL_DUMP:-}" ]; then
  mkdir -p "$(dirname "$OCUDU_UL_DUMP")"
fi

# ---- REFUSE TO START ON TOP OF A STRAY gNB ----------------------------------------------------------
# Measured 2026-09-24: a short gNB started only to read a start-up line (the P0-6 verification) did not
# die on its first kill, stayed as an orphan holding the GTP-U socket, and the next leg failed 15
# seconds in with "Failed to bind UDP socket to 192.168.64.1:2152. Address already in use / Unable to
# allocate the required NG-U network resources". That leg is worthless (no contract report at all) and
# the failure looks like a configuration problem. A stray gNB also competes for the GPU, so an offline
# arm running next to it reads the wrong numbers (5.8.20 (4)). One check here covers both.
# EXACT process names, never a command-line pattern. Measured 2026-09-24, right after this check was
# added: `pgrep -f "apps/gnb/gnb|ul_chain_replay"` matched the very shell that was RUNNING the pattern
# (its own command line contains that string), so the check reported a "stale gNB" that was not a gNB
# and refused a real leg. -x matches the process NAME only, which a shell, an editor, a grep or a
# history echo can never be.
stray=""
for _n in gnb ul_chain_replay; do
  _p=$(pgrep -x "$_n" 2>/dev/null | head -5)
  [ -n "$_p" ] && stray="$stray $_p"
done
if [ -n "$stray" ]; then
  echo "REFUSING to run: another gNB (or a GPU replay) is already running:" >&2
  ps -o pid,etime,command -p $(echo $stray | tr ' ' ',' | sed 's/^,//;s/,$//') 2>/dev/null | sed 's/^/  /' >&2
  echo "  A stale one holds the GTP-U socket (192.168.64.1:2152) and the GPU:" >&2
  echo "    the next leg dies with 'Failed to bind UDP socket ... Address already in use'," >&2
  echo "    and any offline arm run beside it reads the wrong numbers." >&2
  echo "  Fix: kill them (kill -TERM <pid>, then -9 if it survives), then re-run." >&2
  exit 2
fi
if lsof -nP -iUDP:2152 2>/dev/null | tail -n +2 | grep -q .; then
  echo "REFUSING to run: UDP 2152 is already bound (that is the gNB's NG-U socket):" >&2
  lsof -nP -iUDP:2152 2>/dev/null | sed 's/^/  /' >&2
  echo "  Fix: kill the process holding it, then re-run." >&2
  exit 2
fi

if [ "$(id -u)" != "0" ]; then
  echo "run me as root: sudo -E bash $0 $MODE $LABEL $*" >&2
  exit 2
fi

# ---- a gpu leg without its DEVICE KERNELS is not a gpu leg --------------------------------
# The .metallib files are git-ignored build products that live in the SOURCE directories (the engine
# bakes those absolute paths in at configure time and falls back to "next to the executable", then to
# the cwd). A checkout or a git worktree therefore has NONE of them until the metallib targets are
# built, and the failure mode is SILENT: the engine loads no kernel and the run goes down host paths
# while still calling itself mode=gpu. The design record warns about exactly this for copied reference
# binaries (doc_chinese/work_tmp/README.md, "参考二进制必须带着它的 .metallib"); met here for real on
# 2026-09-24 when a fresh worktree was prepared for the pre-merge A/B. Refuse instead of producing a
# leg that would be evidence about the CPU path.
# The list is the LIBRARIES THE ENGINES LOAD, which is where this gate went stale once already:
# metal_kernel_fusion M3 merged the estimator's, the equalizer's and the demapper's three libraries into
# ONE (lib/phy/metal/ocudu_lane.metallib, built by the target ocudu_metallib_lane), and this check kept
# asking for the three files that no longer exist - so it refused every gpu leg (measured 2026-10-07,
# right before the AMC pair, with the merged library sitting right there). One library per STAGE is the
# invariant to keep: the DFT still owns its own, and the whole DFT-grid -> LLR segment shares the lane's.
if [ "$MODE" != "cpu" ]; then
  missing=()
  for k in lib/phy/generic_functions/metal/ocudu_dft.metallib \
           lib/phy/metal/ocudu_lane.metallib; do
    [ -f "$ROOT/$k" ] || missing+=("$k")
  done
  if [ ${#missing[@]} -ne 0 ]; then
    echo "REFUSING to run a $MODE leg: ${#missing[@]} device kernel(s) missing, so the engines would fall" >&2
    echo "back to the host paths SILENTLY while the leg still called itself $MODE:" >&2
    for m in "${missing[@]}"; do echo "  $m" >&2; done
    echo "  Fix: cmake --build build --target ocudu_metallib_dft ocudu_metallib_lane" >&2
    exit 2
  fi
fi

CLI_ARGS=()
REGIME=default
SMOKE=0
# TWO FORMS FOR AN OPTION, because the application accepts both and the leg must not speak a dialect of its own.
# `--option=value` has always been accepted here; `--option value` -- which is how every command line in the docs
# and every gnb invocation outside this script is written -- was REFUSED with "join them with '='". Measured
# 2026-10-16: the operator's own four-slot AI command line mixed both forms, which is exactly what that contract
# produced. Both are taken now, and the rule for the two-word form is arity, not guessing: a `--option` with no
# '=' consumes the NEXT word as its value unless that word is itself a long option, which leaves flags working and
# keeps negative values where they belong (`--expert_phy.p0_nominal_with_grant -76`).
while [ $# -gt 0 ]; do
  kv=$1
  case "$kv" in
    # CONSUMED here, not forwarded: this one is a property of the LEG, not a gNB option. It is written
    # into the leg's own stderr as `[leg] regime=`, which is where wip/milestone_audit.sh reads it back.
    # Why it must be declared rather than derived: the two regimes do not share their criteria. Under a
    # load generator `stale > 0` is the PRE-REGISTERED EXPECTATION (wip/wall_ab.sh's R4: "stale > 0 =>
    # the leg is of the same kind as s70", PASS - 5.9.127), while the default regime's A1-2 C5 demands
    # `stale=0` (5.9.118 (3)). Measured 2026-09-24: the audit auto-picked the newest leg - the overloaded
    # s82 - and reported `stale = 0` and C5 as FAIL on it, because the criterion had been bound to the
    # wrong regime. Classifying a leg by its own `stale` reading would make that criterion unfalsifiable.
    --regime=default) REGIME=default ;;
    --regime=stress)  REGIME=stress ;;
    --regime=*)       echo "refusing '$kv': regime must be default or stress" >&2; exit 2 ;;
    --regime)         shift; [ $# -gt 0 ] || { echo "refusing '--regime': it needs a value (default or stress)" >&2; exit 2; }
                      case "$1" in default|stress) REGIME=$1 ;; *) echo "refusing '--regime $1': regime must be default or stress" >&2; exit 2 ;; esac ;;
    # SMOKE SECONDS: watch the first N seconds for the radio's own failure lines and stop the leg if they appear.
    # Added after p94 (2026-09-28) died of a B200 USB drop: the receive stream had been timing out for ~104 s
    # ("exceeded maximum number of timed out receive calls", 10 x 200 ms trials each) while the DL queue grew to
    # 1000 SDUs, and the leg only ended when an uncaught uhd::usb_error aborted the process - so the operator paid
    # four minutes plus a phone test for a leg that was already void in its first seconds. Default 0 = OFF: this
    # adds no behaviour to any leg that does not ask for it.
    --smoke=*)        SMOKE=${kv#*=}
                      case "$SMOKE" in ''|*[!0-9]*) echo "refusing '$kv': --smoke=SECONDS (a number)" >&2; exit 2 ;; esac ;;
    --smoke)          shift; [ $# -gt 0 ] || { echo "refusing '--smoke': it needs a value in seconds" >&2; exit 2; }
                      SMOKE=$1
                      case "$SMOKE" in ''|*[!0-9]*) echo "refusing '--smoke $SMOKE': --smoke SECONDS (a number)" >&2; exit 2 ;; esac ;;
    --*)              case "$kv" in
                        *=*) CLI_ARGS+=("$kv") ;;
                        *)   # A long option with no '=': take the next word as its value unless that word is itself a
                             # long option (then this one is a flag and stands alone).
                             if [ $# -gt 1 ] && [ "${2#--}" = "$2" ]; then
                               CLI_ARGS+=("$kv" "$2"); shift
                             else
                               CLI_ARGS+=("$kv")
                             fi ;;
                      esac ;;
    OCUDU_*=*)        export "$kv" ;;
    OCUDU_*)          # The same two forms for a knob: OCUDU_NAME=value, or OCUDU_NAME value.
                      CLI_NAME=$kv; shift
                      [ $# -gt 0 ] || { echo "refusing '$CLI_NAME': it needs a value (OCUDU_NAME=value or OCUDU_NAME value)" >&2; exit 2; }
                      case "$1" in --*) echo "refusing '$CLI_NAME $1': the knob has no value" >&2; exit 2 ;; esac
                      export "$CLI_NAME=$1" ;;
    *=*)              echo "refusing '$kv': a knob must be OCUDU_*=…, and a gNB option must start with --" >&2; exit 2 ;;
    # REFUSED, not ignored. This branch used to warn and drop the word, which contradicts the rule stated at
    # the top of this file ("Anything else is refused loudly rather than dropped") and cost a leg: the operator
    # wrote `--log.phy_level debug` as two words, `debug` was dropped, gNB was handed `--log.phy_level` with no
    # value, it consumed the NEXT option as that value and died at startup with "--phy_level: Log level
    # '--expert_phy.phy_pipeline' not supported" (measured 2026-09-22 23:15, leg s55-racefix). That two-word form
    # is accepted since 2026-10-16 (see the loop's header), so a bare word here is now genuinely stray.
    *)                echo "refusing '$kv': neither a knob (OCUDU_NAME=value or OCUDU_NAME value) nor a gNB option" >&2
                      echo "  (--option=value or --option value). A bare word here has no option to belong to." >&2
                      exit 2 ;;
  esac
  shift
done

# Pre-flight: the gNB's own parser is the only authority on which options exist. A leg that dies on
# "The following argument was not expected: --log.sched_level=debug" costs an operator a phone test and
# reports nothing (measured 2026-09-23: that option does not exist in this build - the scheduler logger
# has no per-logger level, see gnb.cpp:133). So the exact argv is handed to the binary with --dryrun and
# refused here rather than discovered on air.
#
# WHAT --dryrun DOES, exactly (verified 2026-09-23, milestone audit): gnb.cpp returns at :281-283
# IMMEDIATELY after CLI11_PARSE, i.e. BEFORE validate_appconfig() and every on_configuration_validation()
# at :291-297. So it checks THE PARSER, not the configuration: an unknown option fails the leg here, but
# a configuration the validators would refuse (e.g. `--phy_pipeline cpu --expert_phy.pusch_dft_type
# metal`, whose rule lives in du_low_phy_pipeline.h:167) passes this pre-flight. That case is still not
# an on-air surprise - the validators run at gnb.cpp:291, before the radio is opened - it costs a start,
# not a phone test. Do not read "the dry run passed" as "the configuration is valid"; the arity of this
# check is the parser's.
if [ ${#CLI_ARGS[@]} -ne 0 ]; then
  dryrun_log=$(mktemp)
  if ! "$ROOT/build/apps/gnb/gnb" -c "$CONFIG" "${CLI_ARGS[@]}" "${MODE_ARGS[@]+"${MODE_ARGS[@]}"}" \
        --expert_phy.phy_pipeline "$MODE" --dryrun >"$dryrun_log" 2>&1; then
    echo "REFUSING to run: the gNB rejected this argument set (dry run):" >&2
    # THE GREP IS A CONVENIENCE, NOT THE EVIDENCE: when it matches nothing the operator is left with a refusal
    # and no reason at all, which is what happened on 2026-10-18 (the gNB failed without printing any of these
    # words, and the only copy was a mktemp path that did not survive). So the tail of the log is ALWAYS shown.
    hits=$(grep -E "not expected|Requires|error|Error|assert|Assert|terminate|what\(\)" "$dryrun_log" | head -5)
    if [ -n "$hits" ]; then
      printf '%s\n' "$hits" | sed 's/^/  /' >&2
    else
      echo "  (no diagnostic line matched; last 6 lines of the log follow)" >&2
      tail -6 "$dryrun_log" | sed 's/^/  /' >&2
    fi
    echo "  (dry run log: $dryrun_log)" >&2
    exit 2
  fi
  rm -f "$dryrun_log"
fi

# The per-module backend knobs are passed for cpu_gpu ONLY, and deliberately not for the other two:
#
#   * cpu      - resolve_phy_pipeline() forces every backend to the CPU for this mode;
#   * cpu_gpu  - the module knobs are the WHOLE POINT of this mode ("each module follows its own
#                backend knob"), and leaving them at "auto" resolves them to CPU (see the cpu_gpu case
#                in resolve_phy_pipeline) - i.e. the same pipeline as `cpu`. A "cpu_gpu" arm that
#                resolves to CPU backends would be comparing the CPU path with itself;
#   * gpu      - the lane owns the backends and resolve_phy_pipeline() fills them in from
#                phy_pipeline_lane_defaults (metal / metal_mmse / metal, device_grid on). Passing them
#                by hand here would re-hide a broken mode resolver behind knobs that happen to be
#                right, which is how the old script could only ever express one mode.
case "$MODE" in
  cpu_gpu)
    # WHICH MODULES ARE OFFLOADED IS SELECTABLE (2026-09-28, the "module ladder"). The default stays exactly what
    # this mode has always passed, so every existing cpu_gpu reading keeps its meaning; the ladder exists because
    # the slow-receive tail turned out to need a Metal path PRESENT rather than busy (dev doc 6.150: cpu 0.002%
    # vs cpu_gpu 0.118% vs gpu 0.058-0.171%, at the same traffic and on one binary), and "which module brings it"
    # cannot be answered from the archive - its cpu_gpu legs all carry the full set.
    #
    # A whitelist rather than free-form options: each arm has to be a configuration resolve_phy_pipeline() accepts,
    # and "auto" resolves to CPU in this mode, so a single-module arm is legal and the others stay on the CPU.
    # The set is echoed into the leg's own stderr (mode options), so an arm states what it offloaded.
    case "${LEG_CG_MODULES:-all}" in
      all)  MODE_ARGS=(
              --expert_phy.pusch_dft_type metal
              --expert_phy.pusch_channel_estimator_algo metal_mmse
              --expert_phy.pusch_channel_equalizer_backend metal
              --expert_phy.pusch_ldpc_decoder_type auto
              --expert_phy.device_resource_grid on
            ) ;;
      none) MODE_ARGS=() ;;   # every module auto -> CPU, grid auto -> host grid: the CPU-equivalent control
      # A '+' SEPARATED COMBINATION of the same primitives, because no SINGLE module reproduces the stalls and the
      # whole set does: measured 2026-09-28 (batch 3) - none 0.001%, grid 0.001%, ce 0.001%, eq 0.001%, dft 0.003%,
      # ALL FOUR 0.111% together with the receive-pool dry-out and a host that waits for the DFT. So the question
      # became "which SUBSET", and `LEG_CG_MODULES=dft+grid+ce` is how it gets asked.
      *)    MODE_ARGS=()
            _mods=()
            IFS='+' read -r -a _mods <<< "${LEG_CG_MODULES}"
            for _m in "${_mods[@]}"; do
              case "$_m" in
                grid) MODE_ARGS+=( --expert_phy.device_resource_grid on ) ;;
                dft)  MODE_ARGS+=( --expert_phy.pusch_dft_type metal ) ;;
                ce)   MODE_ARGS+=( --expert_phy.pusch_channel_estimator_algo metal_mmse ) ;;
                eq)   MODE_ARGS+=( --expert_phy.pusch_channel_equalizer_backend metal ) ;;
                *)    echo "refusing LEG_CG_MODULES='${LEG_CG_MODULES}': '$_m' is not one of grid|dft|ce|eq" >&2
                      echo "  (a combination is written with '+', e.g. dft+grid+ce; 'none' and 'all' stand alone)" >&2
                      exit 2 ;;
              esac
            done
            [ ${#_mods[@]} -gt 0 ] || { echo "refusing empty LEG_CG_MODULES" >&2; exit 2; } ;;
    esac ;;
  *)
    MODE_ARGS=() ;;
esac

# A REPEATED --option IS A SILENT-FAILURE TRAP, refused here (2026-10-06). MODE_ARGS and the caller's own
# --options both reach argv, and gnb's parser rejects a repeat OUTRIGHT, so the process exits before it prints a
# single banner. Measured on p307-cg-metaldec: the caller wanted `--expert_phy.pusch_ldpc_decoder_type=metal` while
# the mode's own `all` set injected `... auto`, gnb answered
#
#   --pusch_ldpc_decoder_type: At Most 1 required but received 2
#
# and died - stdout 42 bytes with no cell line, while the operator watched a phone refuse to attach, toggled
# airplane mode and saw nothing at the core: every symptom of a radio fault, and none of it about the arm. The
# refusal belongs here, where the message can name the workaround.
for _a in ${MODE_ARGS[@]+"${MODE_ARGS[@]}"}; do
  _name=${_a%%=*}
  for _b in ${CLI_ARGS[@]+"${CLI_ARGS[@]}"}; do
    case "$_b" in
      "$_name"|"$_name"=*)
        echo "refusing: '$_name' would be passed twice - once by this mode's module set (LEG_CG_MODULES=${LEG_CG_MODULES:-all}) and once by you." >&2
        echo "  gnb's parser rejects the repeat and exits before any banner (p307-cg-metaldec, 2026-10-06)." >&2
        echo "  Fix: name the modules explicitly instead of 'all' - e.g. LEG_CG_MODULES=dft+ce+eq+grid leaves the decoder out of the set, so your own $_name=... is the only one." >&2
        exit 2 ;;
    esac
  done
done

echo "pipeline mode : $MODE" >&2
echo "mode options  : ${MODE_ARGS[*]:-<none: the mode resolves the backends itself>}" >&2
echo "leg           : $LABEL" >&2
echo "regime        : $REGIME   <- default = no load generator (stale=0 is a criterion) / stress = loaded (stale>0 is expected, 5.9.127 R4)" >&2
echo "cell config   : ${CONFIG#$ROOT/}   <- decides PRB count and slot period" >&2
echo "log (ocudulog): $LOG" >&2
echo "log (stderr)  : $LOG.stderr   <- [phy_pipeline] contract / [ul_host] / [metal_stats] / [ul_gpu_lane]" >&2
echo "log (stdout)  : $LOG.stdout   <- the app's banner, the cell line, radio/AMF messages, validator refusals" >&2
echo "gNB options   : ${CLI_ARGS[*]:-<none>}   <- these reach argv, not the environment" >&2
env | grep -E '^OCUDU_[A-Z0-9_]+=' | sort | sed 's/^/knob          : /' >&2 || true
echo "capture       : ${OCUDU_UL_DUMP_COUNT} receptions, rv ${RV_NOTE}, tb dump ${OCUDU_HELENA_DUMP_DIR}" >&2
echo >&2

# The same provenance is repeated INTO the leg's own stderr file once the tees are up (see below), so a
# leg carries the cell geometry and the knobs it ran with even when the console is gone. The geometry is
# not decoration: "余量" is 1 - residency/slot, and both the PRB count and the slot period come from this
# file (wip/ul_load.sh reads it back out of the cell line).
PROVENANCE=$(printf '[leg] regime=%s\nsmoke test    : %s\npipeline mode : %s\nmode options  : %s\nleg           : %s\ncell config   : %s\ngNB options   : %s\n' \
  "$REGIME" "$([ "$SMOKE" -gt 0 ] && echo "${SMOKE}s (a radio failure line stops the leg)" || echo off)" \
  "$MODE" "${MODE_ARGS[*]:-<none>}" "$LABEL" "${CONFIG#$ROOT/}" "${CLI_ARGS[*]:-<none>}")
# The LEADING newline is load-bearing: $(printf ...) strips the trailing one, so appending directly
# glued the first knob line onto the 'gNB options' line - measured on every leg (s82: 'gNB options   :
# <none>knob          : OCUDU_UL_PHASE_SEGMENTS=1'), which silently defeats any '^knob' grep and made
# the P0 gate judge a real split arm as 'knob off'.
PROVENANCE+=$(printf '\n%s' "$(env | grep -E '^OCUDU_[A-Z0-9_]+=' | sort | sed 's/^/knob          : /' || true)")

cd "$ROOT"
# BOTH streams are teed, and both stay on the terminal. stdout used to go only to the terminal, so a
# leg's file set was missing the startup provenance (the commit banner, the cell line) and, worse,
# every refusal the configuration validators print - those go through fmt::print to stdout, not
# through the logger. Nothing per-hop lives there (the contract and every counter are on stderr, and
# the [phy_pipeline] banner is a logger line in the ocudulog file), but a leg must be readable
# afterwards without asking whoever ran it to paste a console.
#
# The two tees are explicit descriptors that are WAITED FOR, not `> >(tee ...)` process
# substitutions. With the substitution form the shell returns to the prompt as soon as gnb exits and
# leaves the tee children to finish on their own: the last line of the shutdown report then reaches
# the console after the prompt. Measured on the `probe-iq2llr` leg (2026-09-19 22:16): the console
# showed "[ul_rx] blocks=... gaps=0" with the prompt glued to it and the rest of the line arriving
# later, while the .stderr file - which tee had already read out of the pipe - was complete and
# newline-terminated. Waiting for the tees is what makes the console and the files agree; closing the
# descriptors first is what makes the wait return (the tee's stdin then sees EOF).
#
# /!\ STOP THE gNB WITH Ctrl-C (SIGINT), NOT WITH kill (SIGTERM).
# gnb.cpp installs two different handlers: `interrupt_signal_handler` (SIGINT) only clears the
# running flag, so the app leaves its loop and runs the normal shutdown - which is where the
# [phy_pipeline] contract, [ul_host], [metal_stats] and [ul_gpu_lane] blocks are printed.
# `cleanup_signal_handler` (SIGTERM) flushes the logger and exits, skipping all of them.
# Measured: a leg stopped with SIGTERM produced a 20 MB log, an empty .stdout, a .stderr holding
# only the UHD banner, and a report with every counter line blank - the crossing numbers that are
# the whole point of the leg were gone. There is no way to recover them after the fact, because
# nothing writes them to the ocudulog file.
exec 3> >(tee "$LOG.stdout")
out_tee=$!
exec 4> >(tee "$LOG.stderr" >&2)
err_tee=$!
# Repeat the provenance into the leg's own stderr file, so the geometry and the knobs survive the
# console (see PROVENANCE above). It is printed before gnb's first line, so a reader of the file sees
# what the run was without asking for the terminal.
printf '%s\n\n' "$PROVENANCE" >&4
# --log.all_level is the script's DEFAULT, not its policy: when the caller asked for a level (the
# scheduler's LA line needs debug, and this build has no per-logger option for it), the caller's value
# must win - appending it after theirs would silently override it, which is the same class of bug this
# script's argument loop refuses bare words for.
LOG_LEVEL_ARGS=(--log.all_level info)
for kv in "${CLI_ARGS[@]+"${CLI_ARGS[@]}"}"; do
  case "$kv" in
    --log.all_level=*) LOG_LEVEL_ARGS=() ;;
  esac
done

# JOB CONTROL ON, so the gNB lands in its OWN process group. WHY: Ctrl+C is delivered by the terminal to the whole
# FOREGROUND PROCESS GROUP, and without job control the backgrounded gNB is in this script's group -- so one key
# press reaches the gNB twice, once from the terminal and once from the trap below, and two signals from one press
# are indistinguishable from "the operator pressed again". Measured 2026-10-16 (leg aillrr-256qam-ai_1016_1804): the
# second delivery forced the exit, the report block was truncated and the guard below printed "THIS LEG HAS NO
# COMPLETE REPORT". signal_handling.cpp also ignores duplicates inside a two-second window now; this removes the
# duplicate at its source, so the single signal the trap sends is the one the app acts on.
set -m
./build/apps/gnb/gnb -c "$CONFIG" \
  "${CLI_ARGS[@]+"${CLI_ARGS[@]}"}" \
  "${MODE_ARGS[@]+"${MODE_ARGS[@]}"}" \
  --expert_phy.phy_pipeline "$MODE" \
  "${LOG_LEVEL_ARGS[@]}" \
  --log.filename "$LOG" \
  0</dev/null >&3 2>&4 &
GNB_PID=$!
# STDIN IS /dev/null, AND THAT IS NOT COSMETIC. gnb registers STDIN_FILENO with its command dispatcher and reads it
# asynchronously. This process is a BACKGROUND job (job control is on above, so it has its own process group), and the
# kernel sends SIGTTIN -- stop -- to a background process that reads its controlling terminal. So a single keystroke in
# the terminal froze a leg mid-run: measured 2026-10-29 on leg w2_harq_mcs02_ping_1028f, the process sat in state T with
# rc=149 (128+21, SIGTTIN) after printing every sched-mcs line for half an hour, the wrapper waited forever, and the
# leg's shutdown counters never printed, so the guard below refused it as a result. The console's only jobs are `h` and
# `q`, which nobody uses during a flight; /dev/null gives it EOF once, `parse_stdin` logs "Connection closed" and returns,
# and no read can ever reach the tty again.
# ONE Ctrl-C MUST STOP gnb AND NOT THIS SCRIPT. Until 2026-09-28 gnb ran in the FOREGROUND, and the script survived
# a Ctrl-C only because bash ignores SIGINT in itself while it waits for a foreground job. Holding the pid (which
# the smoke test below needs) turned that into `wait` on a BACKGROUND job, where that protection does NOT apply:
# the script would die with gnb, skipping the end-of-leg report check - the very guard that says "do not read this
# leg". So the contract is explicit now instead of inherited: the trap stops gnb and lets this script finish.
stop_gnb() { [ -n "${GNB_PID:-}" ] && kill -INT "$GNB_PID" 2>/dev/null; }
trap stop_gnb INT
SMOKE_PID=""
if [ "$SMOKE" -gt 0 ]; then
  (
    for ((si = 1; si <= SMOKE; ++si)); do
      sleep 1
      if grep -aqE 'exceeded maximum number of timed out receive calls|failed receiving packet|usb_error|LIBUSB_ERROR' \
           "$LOG.stdout" "$LOG.stderr" 2>/dev/null; then
        echo >&2
        echo "!! RADIO SMOKE TEST: the receive stream is failing after ${si}s (see above) - stopping this leg NOW." >&2
        echo "   It would produce no report anyway (see dev doc 6.150 (4b): p94 died this way after ~104 s of silence)." >&2
        echo "   Fix the radio (power-cycle the B200, uhd_find_devices), then re-run." >&2
        kill -INT "$GNB_PID" 2>/dev/null
        exit 0
      fi
    done
  ) &
  SMOKE_PID=$!
fi
# A HEARTBEAT, BECAUSE ONE LINE AT RECEPTION 100000 IS NOT ENOUGH. ul_capture prints "budget exhausted" once, to stderr, and then falls
# silent while the leg keeps running: the scheduler's own [sched-mcs] lines continue, so the console still looks busy and the operator has no way
# to tell a finished capture from a working one. Measured 2026-10-29 on leg w2_harq_QPSK_iperf_1029d: the message was there, at line 508 of 597,
# between two grant lines -- and the operator, watching that console, could not find it.
# So say it every minute, in the runner's own voice, whether the answer is good or bad.
CAPTURE_DIR=$(dirname "$OCUDU_UL_DUMP")
(
  while sleep 60; do
    n=$(find "$CAPTURE_DIR" -maxdepth 1 -name 'cap_*.bin' ! -name '*_h.bin' ! -name '*_llr.bin' 2>/dev/null | wc -l | tr -d ' ')
    if grep -aq "budget exhausted" "$LOG.stderr" "$LOG" 2>/dev/null; then
      echo "[capture] ${n} file(s) -- BUDGET SPENT: the leg is still running but nothing more will be written. Restart it to continue." >&2
    elif [ "$n" -eq 0 ]; then
      echo "[capture] 0 file(s) -- NOTHING IS BEING WRITTEN (check the capture plan above and the phone)." >&2
    else
      echo "[capture] ${n} file(s) -- writing" >&2
    fi
  done
) &
HEARTBEAT_PID=$!

wait "$GNB_PID"
rc=$?
kill "$HEARTBEAT_PID" 2>/dev/null
if [ -n "$SMOKE_PID" ]; then kill "$SMOKE_PID" 2>/dev/null; wait "$SMOKE_PID" 2>/dev/null; fi
exec 3>&- 4>&-
wait "$out_tee" "$err_tee"

# ---- did the leg actually produce a REPORT? ----------------------------------------------------
# Every counter this work is judged by - the [phy_pipeline] contract (which carries the dft
# radio-inputs line and its "Plain route by why:" instrument), [ul_host], [metal_stats],
# [ul_gpu_lane] - is printed by the SHUTDOWN path and by nothing else. A process that leaves
# without it (SIGKILL, SIGHUP from a closed terminal, a kill instead of Ctrl-C) leaves a leg
# whose files look plausible - a big .log, a .stderr with the startup diagnostics - and whose
# numbers simply DO NOT EXIST: they live in process memory, and nothing recovers them.
#
# The check is worth having because the OTHER way to reach the same-looking files is a RACE, and
# that is what happened on 2026-09-23 (leg s69-a12-n78): the operator stopped the gNB correctly
# with Ctrl-C, the shutdown report was still draining to the tees, and a reader who looked at
# .stderr in that window saw the startup lines and nothing else. The reading was retracted a
# minute later when the block landed - but the same confusion in the other direction ("the file
# is there, so the numbers are in it") is exactly how a reportless leg gets read as "all zeros".
#
# So say it HERE, while the operator is still sitting in front of the terminal, and exit
# non-zero: a reportless leg is not a leg, and it must not be read later as "the counters were
# zero" (§5.9.97: "cannot read" is RED, never absent). The startup tees are already closed, so
# the message goes to the real stderr.
# WHETHER THE CAPTURE ACTUALLY KEPT WHAT IT WROTE (design section 19.621). Until 2026-10-26 the capture name was made
# from the system frame number and the slot index, and the frame number repeats every 1024 frames -- 10.24 seconds at
# 30 kHz -- so a longer leg overwrote its own earlier captures and left exactly one frame cycle on disk. Measured: twenty
# thousand announced, five thousand nine hundred and twenty-eight files. The names are collision-free now, and this check
# is what proves it on every leg instead of on the one that was looked at.
echo >&2
echo "-- capture yield --" >&2
CAPS=$(find "$(dirname "$OCUDU_UL_DUMP")" -maxdepth 1 -name 'cap_*.bin' ! -name '*_h.bin' ! -name '*_llr.bin' 2>/dev/null | wc -l | tr -d ' ')
ANNOUNCED=$(grep -ah "\[ul_capture\] heartbeat:" "$LOG.stderr" "$LOG" 2>/dev/null | tail -1 | sed 's/.*heartbeat: \([0-9]*\) capture(s).*/\1/')
BUDGET=$(grep -ah "\[ul_capture\] plan:" "$LOG.stderr" "$LOG" 2>/dev/null | tail -1 | sed 's/.*budget \([0-9]*\) reception.*/\1/')
SPAN=$(find "$(dirname "$OCUDU_UL_DUMP")" -maxdepth 1 -name 'cap_*.bin' ! -name '*_h.bin' ! -name '*_llr.bin' 2>/dev/null | sed 's|.*/cap_||;s|_.*||' | sort -n | awk 'NR==1{mn=$1} {mx=$1} END{print (mx-mn)}')
echo "   files on disk: ${CAPS:-0}   last heartbeat: ${ANNOUNCED:-none}   budget: ${BUDGET:-none}   sfn span: ${SPAN:-0} frames" >&2
# `OCUDU_UL_DUMP` is a FILE PREFIX, not a directory: the captures land beside it as <prefix>_<fram...>. Counting inside the
# prefix path itself finds nothing and reported a clean leg as total loss (measured 2026-10-26 18:35).
if [ -n "$ANNOUNCED" ] && [ "${CAPS:-0}" -lt "$ANNOUNCED" ]; then
  echo "!! CAPTURE LOSS: ${ANNOUNCED} announced, ${CAPS} on disk -- names are colliding again (see 19.621)." >&2
  CAPTURE_VERDICT=1
elif [ "${SPAN:-0}" -le 1024 ]; then
  echo "   note: the frame span is ${SPAN:-0}, i.e. within one 1024-frame cycle -- the leg was shorter than 10.24 s," >&2
  echo "         so this leg does not exercise the overwrite fix either way." >&2
else
  echo "   OK: every announced capture is on disk and the span exceeds one frame cycle." >&2
fi

if ! grep -q "\[phy_pipeline\] contract" "$LOG.stderr" 2>/dev/null ||
   ! grep -q "\[metal_stats\]" "$LOG.stderr" 2>/dev/null; then
  echo >&2
  echo "!! THIS LEG HAS NO COMPLETE REPORT - DO NOT READ IT AS A RESULT !!" >&2
  echo "   required: a '[phy_pipeline] contract' line AND the '[metal_stats]' blocks." >&2
  echo "   The contract prints EARLY in the shutdown; the counters this line's claims are judged by" >&2
  echo "   ([metal_stats] dft/handover/burst/mmse_ce, [ul_host], [ul_gpu_lane], [ul_pipeline]) come AFTER" >&2
  echo "   it. Measured 2026-09-24 (leg s76-wall-premerge-n78): the app did not stop within 5 s, the runner" >&2
  echo "   forced the exit, the contract line WAS there, and every counter after it was gone - a guard that" >&2
  echo "   only asked for the contract line called that leg readable." >&2
  if grep -q "Could not stop application" "$LOG.stderr" 2>/dev/null; then
    echo "   this leg printed 'Could not stop application after 5 seconds. Forcing exit.'" >&2
    echo "   (lib/support/signal_handling.cpp) - something did not leave its loop; that is a finding in itself." >&2
  fi
  if [ "$rc" -ne 0 ]; then
    echo "   the gNB exited with rc=$rc (startup/run failure, or a forced exit)" >&2
  else
    echo "   usual cause: the gNB was killed instead of interrupted, or its shutdown did not complete" >&2
  fi
  echo "   Fix: re-run and stop it with ONE Ctrl-C, then WAIT for the whole report block to finish printing." >&2
  if [ "$rc" -ne 0 ]; then exit "$rc"; fi
  exit 3
fi
exit "$rc"
