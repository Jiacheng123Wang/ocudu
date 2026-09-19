#!/usr/bin/env bash
# One on-air leg of the UL/cpu_gpu configuration, with the fusion knob and the log name spelled out.
#
# Why a script: the leg has to be launched as root (-E keeps the OCUDU_* knobs), the log file name must
# carry the leg and the knob state (--log.filename does NOT create the parent directory, it fopen()s),
# and the criteria are read out of that log afterwards. Typing the line by hand is how a leg ends up
# with the wrong knob or an overwritten log.
#
# usage: sudo -E bash wip/run_air_leg.sh <leg-label> [OCUDU_*=value ...]
#   e.g. sudo -E bash doc_chinese/full_gpu_chain/wip/run_air_leg.sh fence OCUDU_UL_FRONTEND_FENCE=1
#        sudo -E bash doc_chinese/full_gpu_chain/wip/run_air_leg.sh baseline
#
# The knobs go in as ARGUMENTS, and the script exports them itself before running the gNB. A bare
# KEY=VALUE after the command name would normally be just a positional parameter, and the script turns
# it into an environment variable on purpose: under sudo the alternative (writing the assignment before
# `bash`, i.e. `sudo -E KEY=VALUE bash ...`) depends on the sudoers policy - env_reset and env_keep can
# strip it even with -E - while a variable exported INSIDE the root process cannot be stripped by any
# policy. Both forms are accepted and both reach the gNB; the header printed below lists what is
# actually in effect, so a leg never has to be trusted from memory.
set -u
LABEL=${1:?leg label, e.g. "fused" or "baseline"}
shift || true

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
CONFIG=$ROOT/configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml
LOGDIR=$ROOT/doc_chinese/work_tmp/logs
LOG=$LOGDIR/gnb_${LABEL}_$(date +%m%d_%H%M).log

mkdir -p "$LOGDIR"     # --log.filename never creates it, and a missing one fails silently
if [ "$(id -u)" != "0" ]; then
  echo "run me as root: sudo -E bash $0 $LABEL $*" >&2
  exit 2
fi

# Two kinds of argument, and they take different paths into the process:
#   * OCUDU_*=…            the fusion/tuning knobs, which the gNB reads from the ENVIRONMENT (under
#                          sudo, an assignment written before `bash` can be stripped by env_reset, so
#                          they are exported INSIDE the root process instead);
#   * anything starting --  a real command-line option (e.g. --cell_cfg.pucch.max_consecutive_kos=300),
#                          which has to reach the gNB's argv.
# The first version of this loop exported everything containing '=', so a --cell_cfg... argument became
# a meaningless shell variable and never reached the gNB: the leg then ran with the DEFAULT and its log
# looked like the knob had no effect (measured: "Cause: 100 consecutive undecoded CSIs" with
# max_consecutive_kos=300 requested). Anything that is neither is refused loudly rather than dropped.
CLI_ARGS=()
for kv in "$@"; do
  case "$kv" in
    --*)                  CLI_ARGS+=("$kv") ;;
    OCUDU_*=*)            export "$kv" ;;
    *=*)                  echo "refusing '$kv': a knob must be OCUDU_*=…, and a gNB option must start with --" >&2; exit 2 ;;
    *)                    echo "ignoring non-knob argument: $kv" >&2 ;;
  esac
done

echo "leg          : $LABEL" >&2
echo "log (ocudulog): $LOG            <- crc=OK/KO, Real-time failure in RF, the ^C series as log lines" >&2
echo "log (stderr)  : $LOG.stderr     <- [metal_stats] / [ul_gpu_lane] / [mmse_time_sum] / contract table" >&2
echo "gNB options  : ${CLI_ARGS[*]:-<none>}   <- these reach argv, not the environment" >&2
echo "OCUDU_CE_LANE_ORDER=${OCUDU_CE_LANE_ORDER:-<default: event>}  OCUDU_CE_FUSED_BURST=${OCUDU_CE_FUSED_BURST:-<unset>}  OCUDU_DFT_OPEN_BLOCK=${OCUDU_DFT_OPEN_BLOCK:-<default: on>}  OCUDU_UL_FRONTEND_FENCE=${OCUDU_UL_FRONTEND_FENCE:-<default: off>}  OCUDU_UL_RX_SYMBOLS=${OCUDU_UL_RX_SYMBOLS:-<unset>}" >&2
# Every OCUDU_* the gNB will inherit, whatever way it was set (argument or environment): the leg's
# evidence has to say which configuration produced it.
env | grep -E '^OCUDU_[A-Z0-9_]+=' | sort | sed 's/^/knob         : /' >&2 || true
echo "stop it with ^C: the UL series, the contract table and the [metal_stats]/[ul_gpu_lane] lines are" >&2
echo "printed as soon as the stop is requested (they used to be printed at the very end, and were lost" >&2
echo "whenever the shutdown crashed)." >&2
echo >&2

cd "$ROOT"
# The counter lines ([metal_stats], [ul_gpu_lane], [mmse_time_sum]) and the contract table go to STDERR,
# not to the ocudulog file, so both streams have to be kept: tee leaves them on the terminal (the ^C
# evidence is still visible) and lands them in $LOG.stderr for the verdict afterwards.
./build/apps/gnb/gnb -c "$CONFIG" \
  "${CLI_ARGS[@]+"${CLI_ARGS[@]}"}" \
  --expert_phy.phy_pipeline cpu_gpu \
  --expert_phy.pusch_channel_estimator_algo metal_mmse \
  --expert_phy.pusch_channel_equalizer_backend metal \
  --expert_phy.pusch_dft_type metal \
  --expert_phy.pusch_ldpc_decoder_type auto \
  --expert_phy.device_resource_grid on \
  --log.all_level info \
  --log.filename "$LOG" \
  2> >(tee "$LOG.stderr" >&2)
