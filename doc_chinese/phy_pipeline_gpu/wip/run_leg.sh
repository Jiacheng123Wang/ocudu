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

mkdir -p "$LOGDIR"     # --log.filename never creates it, and a missing one fails silently
if [ "$(id -u)" != "0" ]; then
  echo "run me as root: sudo -E bash $0 $MODE $LABEL $*" >&2
  exit 2
fi

CLI_ARGS=()
for kv in "$@"; do
  case "$kv" in
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
PROVENANCE=$(printf 'pipeline mode : %s\nmode options  : %s\nleg           : %s\ncell config   : %s\ngNB options   : %s\n' \
  "$MODE" "${MODE_ARGS[*]:-<none>}" "$LABEL" "${CONFIG#$ROOT/}" "${CLI_ARGS[*]:-<none>}")
PROVENANCE+=$(env | grep -E '^OCUDU_[A-Z0-9_]+=' | sort | sed 's/^/knob          : /' || true)

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
./build/apps/gnb/gnb -c "$CONFIG" \
  "${CLI_ARGS[@]+"${CLI_ARGS[@]}"}" \
  "${MODE_ARGS[@]+"${MODE_ARGS[@]}"}" \
  --expert_phy.phy_pipeline "$MODE" \
  --log.all_level info \
  --log.filename "$LOG" \
  >&3 2>&4
rc=$?
exec 3>&- 4>&-
wait "$out_tee" "$err_tee"
exit "$rc"
