#!/usr/bin/env bash
# One on-air leg of the fused-lane work, with the PIPELINE MODE as a required argument.
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
set -u
MODE=${1:?pipeline mode: cpu, cpu_gpu or gpu}
LABEL=${2:?leg label, e.g. "s1-unlock" or "baseline"}
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
CONFIG=${LEG_CONFIG:-$ROOT/configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml}
if [ ! -f "$CONFIG" ]; then
  echo "REFUSING to run: LEG_CONFIG=$CONFIG is not a file." >&2
  exit 2
fi
LOGDIR=$ROOT/doc_chinese/phy_pipeline_gpu/wip/logs
# The mode is part of the file name: an A/B across modes must never be able to overwrite one arm
# with the other, which is the mistake that makes a comparison silently compare a run with itself.
LOG=$LOGDIR/gnb_${MODE}_${LABEL}_$(date +%m%d_%H%M).log

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
  echo "  Fix (then re-run):  touch build/hashes.h && cmake --build build --target gnb" >&2
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

mkdir -p "$LOGDIR"     # --log.filename never creates it, and a missing one fails silently

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
if [ "$MODE" != "cpu" ]; then
  missing=()
  for k in lib/phy/generic_functions/metal/ocudu_dft.metallib \
           lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_mmse.metallib \
           lib/phy/upper/channel_processors/metal/ocudu_equalizer.metallib \
           lib/phy/upper/channel_modulation/metal/ocudu_demod.metallib; do
    [ -f "$ROOT/$k" ] || missing+=("$k")
  done
  if [ ${#missing[@]} -ne 0 ]; then
    echo "REFUSING to run a $MODE leg: ${#missing[@]} device kernel(s) missing, so the engines would fall" >&2
    echo "back to the host paths SILENTLY while the leg still called itself $MODE:" >&2
    for m in "${missing[@]}"; do echo "  $m" >&2; done
    echo "  Fix: cmake --build build --target ocudu_metallib_dft ocudu_metallib_demod ocudu_metallib_equalizer ocudu_mmse_metallib" >&2
    exit 2
  fi
fi

CLI_ARGS=()
REGIME=default
for kv in "$@"; do
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
    --*)                  CLI_ARGS+=("$kv") ;;
    OCUDU_*=*)            export "$kv" ;;
    *=*)                  echo "refusing '$kv': a knob must be OCUDU_*=…, and a gNB option must start with --" >&2; exit 2 ;;
    # REFUSED, not ignored. This branch used to warn and drop the word, which contradicts the rule stated at
    # the top of this file ("Anything else is refused loudly rather than dropped") and cost a leg: the operator
    # wrote `--log.phy_level debug` as two words, `debug` was dropped here, gNB was handed `--log.phy_level`
    # with no value, it consumed the NEXT option as that value and died at startup with
    #   --phy_level: Log level '--expert_phy.phy_pipeline' not supported
    # leaving a report in which every counter was zero (measured 2026-09-22 23:15, leg s55-racefix). The
    # option=value form is this loop's contract, so say the fix instead of dropping the half that matters.
    *)                    echo "refusing '$kv': neither a knob (OCUDU_*=value) nor a gNB option (--option=value)." >&2
                          echo "  A bare word here is almost always the VALUE of the option before it: this loop" >&2
                          echo "  reads one word at a time, so '--log.phy_level debug' arrives as the option plus" >&2
                          echo "  this word. Join them with '=':   --log.phy_level=debug" >&2
                          exit 2 ;;
  esac
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
    grep -E "not expected|Requires|error|Error" "$dryrun_log" | head -5 | sed 's/^/  /' >&2
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
    MODE_ARGS=(
      --expert_phy.pusch_dft_type metal
      --expert_phy.pusch_channel_estimator_algo metal_mmse
      --expert_phy.pusch_channel_equalizer_backend metal
      --expert_phy.pusch_ldpc_decoder_type auto
      --expert_phy.device_resource_grid on
    ) ;;
  *)
    MODE_ARGS=() ;;
esac

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
echo >&2

# The same provenance is repeated INTO the leg's own stderr file once the tees are up (see below), so a
# leg carries the cell geometry and the knobs it ran with even when the console is gone. The geometry is
# not decoration: "余量" is 1 - residency/slot, and both the PRB count and the slot period come from this
# file (wip/ul_load.sh reads it back out of the cell line).
PROVENANCE=$(printf '[leg] regime=%s\npipeline mode : %s\nmode options  : %s\nleg           : %s\ncell config   : %s\ngNB options   : %s\n' \
  "$REGIME" "$MODE" "${MODE_ARGS[*]:-<none>}" "$LABEL" "${CONFIG#$ROOT/}" "${CLI_ARGS[*]:-<none>}")
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

./build/apps/gnb/gnb -c "$CONFIG" \
  "${CLI_ARGS[@]+"${CLI_ARGS[@]}"}" \
  "${MODE_ARGS[@]+"${MODE_ARGS[@]}"}" \
  --expert_phy.phy_pipeline "$MODE" \
  "${LOG_LEVEL_ARGS[@]}" \
  --log.filename "$LOG" \
  >&3 2>&4
rc=$?
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
