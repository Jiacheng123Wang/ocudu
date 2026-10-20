#!/usr/bin/env bash
# One leg of the c32dmrs batch, with every path resolved from the repository root.
#
# WHY IT EXISTS. The hand-written commands carried `$R`, `$C2`, `$B` and `$NEW` from an earlier
# message, assuming the same shell still held them and that the working directory was the repository
# root. On 2026-10-19 07:58 that produced `bash: gpu: No such file or directory` -- `$R` had gone, so
# bash took `gpu` for the script name -- and the identical mistake with `$B`/`$C2` would have started
# a leg with no config and a capture prefix of `/pat/c4/C`. A command an operator has to keep in sync
# with variables from another window is a command that will eventually start the wrong leg.
#
# So: this resolves everything itself, creates the capture directory the leg needs (fopen does not),
# refuses when something is missing, and can print the exact command instead of running it.
#
# The bundles:
#   a = the retrained c32 (data/runs/d3_dmrs2711_c32)   the first candidate
#   d = the high-SNR c32 WITH the calibration declared (data/runs/d3_dmrs2711h_c32, *_cal)
#   x = an AI arm that names NO bundle, so the compiled-in default binds -- the check that the default
#       change actually reaches the receive path (design section 19.225(e))
#   b = the deployed ft (data/models)                   the reference AI arm
#   c = the classical chain                             the reference the scheduler has always used
#
# `d` is resolved through $CAND so a newer candidate does not need this file edited: CAND=<bundle>
# overrides it. A bundle only becomes the default by a separate, deliberate change.
#
# The PIPELINE variable selects the UL PHY pipeline, which is a SEPARATE axis from the receiver backend
# and is the point of the cpu/gpu/ai comparison:
#   PIPELINE=cpu   the whole uplink PHY on the CPU (every module a CPU backend)
#   PIPELINE=gpu   the fused Metal lane: DFT, channel estimation, equalisation and demapping on the
#                  device, IQ in and LLR out in one lane
#   (default) gpu
# Note the engine refuses --phy_pipeline cpu_gpu together with --pusch_receiver_backend ai: the arm
# replaces the estimator, the equaliser and the demapper, so there is no per-module split to make.
#
# Each leg records 60 uplink captures into <batch>/pat/<short>/ for one purpose: the DM-RS pattern a
# leg ran is not in its log, and the arm's outcome on this cell is decided by that pattern (design
# section 19.211), so a batch whose arms ran different patterns is not a comparison of arms.
#
# Usage:
#   sudo -E bash fly_arm.sh <a|b|c> <label>            e.g. fly_arm.sh c c32dmrs-c4
#   bash fly_arm.sh c c32dmrs-c4 --dry-run             prints the command, needs no root
#   B=<batch> CFG=<yaml> CAPTURES=<n>                  overrides
set -u -o pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
W=$ROOT/doc_chinese/llr_ai_detection/work_tmp
RUNNER=$ROOT/doc_chinese/phy_pipeline_gpu/wip/run_leg.sh
CONFIG=${CFG:-$ROOT/configs/gnb_rf_b200_tdd_n78_20mhz.yml}
BATCH=${B:-$W/legs/c32dmrs}
NEW=$W/data/runs/d3_dmrs2711_c32/d3_dmrs2711_c32.mlmodelc
CAND=${CAND:-$W/data/runs/d3_dmrs2711h_c32/d3_dmrs2711h_c32_cal.mlmodelc}
FT=$W/data/models/d3_modcond_ft.mlmodelc
CAPTURES=${CAPTURES:-60}
PIPELINE=${PIPELINE:-gpu}
# The tree already carries the instrument a cpu/gpu/ai latency comparison needs: OCUDU_RECEIVER_PROBE
# brackets the two depth-3 entry points and prints, at exit, the cost of each and of the pair per
# reception ("[receiver_seam] ... unit=...ms over N reception(s), X ms each"). It is off by default and
# costs one predictable branch per call when on. Set PROBE=0 to disable.
PROBE=${PROBE:-1}

ARM=${1:-}
LABEL=${2:-}
DRY=${3:-}
if [ -z "$ARM" ] || [ -z "$LABEL" ]; then
  echo "usage: sudo -E bash $0 <a|b|c|d|x> <label> [--dry-run]" >&2
  exit 2
fi

case "$ARM" in
  a) FLAGS=(--expert_phy.pusch_receiver_backend=ai --expert_phy.pusch_receiver_model="$NEW") ;;
  d) FLAGS=(--expert_phy.pusch_receiver_backend=ai --expert_phy.pusch_receiver_model="$CAND") ;;
  x) FLAGS=(--expert_phy.pusch_receiver_backend=ai) ;;
  b) FLAGS=(--expert_phy.pusch_receiver_backend=ai --expert_phy.pusch_receiver_model="$FT") ;;
  c) FLAGS=(--expert_phy.pusch_receiver_backend=classic) ;;
  *) echo "arm '$ARM' is not a|b|c|d|x" >&2; exit 2 ;;
esac

missing=0
for path in "$RUNNER" "$CONFIG"; do
  [ -f "$path" ] || { echo "MISSING: $path" >&2; missing=1; }
done
if [ "$ARM" = "a" ] || [ "$ARM" = "b" ] || [ "$ARM" = "d" ]; then   # x deliberately checks nothing
  bundle=${FLAGS[1]#--expert_phy.pusch_receiver_model=}
  [ -d "$bundle" ] || { echo "MISSING bundle: $bundle" >&2; missing=1; }
fi
[ "$missing" = 0 ] || exit 2

SHORT=${LABEL#*-}
PATTERN_DIR=$BATCH/pat/$SHORT
# Created HERE, and before the leg: fopen does not create directories, and a capture that silently
# writes nothing is the failure this workflow has met twice (the leg looks healthy, the corpus is
# empty). Not created in a dry run, which runs as the invoking user while the batch is root-owned.
[ "$DRY" = "--dry-run" ] || mkdir -p "$PATTERN_DIR"

echo "arm $ARM -> $LABEL"
echo "  config   : $CONFIG"
echo "  log dir  : $BATCH"
echo "  captures : $PATTERN_DIR/C (budget $CAPTURES, for the DM-RS pattern only)"
echo "  flags    : ${FLAGS[*]}"

if [ "$DRY" = "--dry-run" ]; then
  echo
  echo "would run:"
  echo "  LEG_CONFIG=$CONFIG LEG_LOGDIR=$BATCH OCUDU_UL_DUMP=$PATTERN_DIR/C OCUDU_UL_DUMP_COUNT=$CAPTURES \\"
  echo "  OCUDU_RECEIVER_PROBE=$PROBE \\"
  echo "    bash $RUNNER $PIPELINE $LABEL ${FLAGS[*]}"
  exit 0
fi

if [ "$(id -u)" != 0 ]; then
  echo "run me as root: sudo -E bash $0 $ARM $LABEL" >&2
  exit 2
fi

# EXTRA_FLAGS appends expert-PHY flags this file does not know about, so a one-off arm does not need the script
# edited (added 2026-10-20 for --expert_phy.device_resource_grid, which the pure configuration needs).
LEG_CONFIG=$CONFIG \
LEG_LOGDIR=$BATCH \
OCUDU_UL_DUMP=$PATTERN_DIR/C \
OCUDU_UL_DUMP_COUNT=$CAPTURES \
OCUDU_RECEIVER_PROBE=$PROBE \
  bash "$RUNNER" "$PIPELINE" "$LABEL" "${FLAGS[@]}" ${EXTRA_FLAGS:-}
