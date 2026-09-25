#!/usr/bin/env bash
# Build a leg-config ARM from the DELIVERY config, moving exactly ONE line, and print the diff.
#
# WHY THIS EXISTS. An arm is evidence about ONE variable, so the arm's config must differ from the delivery
# config by exactly the variable under test - and that has to be checkable, not asserted. A hand-copied config
# file drifts silently the moment the delivery config changes (and the leg would then read as a clean A/B of
# one variable while it moved several). Generating it and PRINTING THE DIFF puts the variable in the leg's own
# transcript: `run_leg.sh` copies the config into the leg log, and this script's output shows what was moved.
#
# The two arms are the transport-side ones of dev doc 6.54 (C):
#   bigframe  recv_frame_size=16384  - the SAME waveform, bigger USB transfers: fewer per-transfer costs, and
#                                      a stall is paid in bigger chunks. The clean transport arm.
#   sc8       otw_format: sc8        - HALF the wire rate per sample (8-bit I/Q instead of 12). It changes the
#                                      waveform, so it is judged on the TRANSPORT readings (recv/slip/
#                                      rx_overflows/load1), never on V1-V5.
#
# usage: bash mk_arm_cfg.sh <bigframe|sc8> [output-path]
#        default output: doc_chinese/work_tmp/arm_<name>.yml   (git-ignored, like the other dev products)
set -eu

ARM=${1:?arm name: bigframe or sc8}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
SRC="$ROOT/configs/gnb_rf_b200_tdd_n78_20mhz.yml"
OUT=${2:-$ROOT/doc_chinese/work_tmp/arm_${ARM}.yml}

[ -f "$SRC" ] || { echo "missing delivery config: $SRC" >&2; exit 2; }
mkdir -p "$(dirname "$OUT")"

case "$ARM" in
  bigframe)
    # The device args line carries the ring and the frame size; only the frame size moves.
    sed -E 's/^( *device_args: .*num_send_frames=[0-9]+)(.*)$/\1,recv_frame_size=16384\2/' "$SRC" > "$OUT"
    want="recv_frame_size=16384"
    ;;
  sc8)
    sed -E 's/^( *otw_format: *)sc12 *$/\1sc8/' "$SRC" > "$OUT"
    want="otw_format: sc8"
    ;;
  *)
    echo "unknown arm '$ARM': expected bigframe or sc8" >&2
    exit 2
    ;;
esac

DIFF=$(diff -u "$SRC" "$OUT" || true)
N_CHANGED=$(printf '%s\n' "$DIFF" | grep -cE '^[+-][^+-]' || true)
if [ "$N_CHANGED" != "2" ]; then
  echo "REFUSING: the arm is not a one-line change ($N_CHANGED changed lines, expected 2: one removed, one added)." >&2
  printf '%s\n' "$DIFF" >&2
  exit 2
fi
if ! grep -qF -- "$want" "$OUT"; then
  echo "REFUSING: the arm does not carry '$want' - the substitution did not apply." >&2
  exit 2
fi

echo "# arm '$ARM' generated from $(basename "$SRC") - the variable it moves:"
printf '%s\n' "$DIFF" | grep -E '^[+-][^+-]' | sed 's/^/  /'
echo "# file: $OUT"
echo "# fly it with:  LEG_CONFIG=$OUT"
