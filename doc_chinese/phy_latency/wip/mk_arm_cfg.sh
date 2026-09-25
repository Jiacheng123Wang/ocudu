#!/usr/bin/env bash
# Build a leg-config ARM from the DELIVERY config, moving exactly ONE line, and print the diff.
#
# WHY THIS EXISTS. An arm is evidence about ONE variable, so the arm's config must differ from the delivery
# config by exactly the variable under test - and that has to be checkable, not asserted. A hand-copied config
# file drifts silently the moment the delivery config changes (and the leg would then read as a clean A/B of
# one variable while it moved several). Generating it and PRINTING THE DIFF puts the variable in the leg's own
# transcript: `run_leg.sh` copies the config into the leg log, and this script's output shows what was moved.
#
# The transport-side arms of dev doc 6.54 (C), as the bench probe left them (dev doc 6.55 (3)):
#   sc8       otw_format: sc8        - HALF the wire rate per sample (8-bit I/Q instead of 12). It changes the
#                                      waveform, so it would be judged on the TRANSPORT readings only - and the
#                                      probe has since answered its question (the wire is not the constraint),
#                                      so it is offered, not recommended.
#   bigframe  RETIRED - REFUSED. `recv_frame_size` above ~8 KB collapses the B200's receive transport on this
#             host: measured with wip/uhd_rx_health, 6 s each, RX+TX, delivery args otherwise -
#               8192 / 8200 -> 100% of the time streamed, 0 radio errors
#               12288       ->  9.8%, every block `overflow`
#               16360       ->  9.8%, every block `overflow`   (UHD clamps a request of 16384 to 16360)
#             It was flown once as `p36-n78-bigframe` before it was measured: the UE could not attach at all
#             (628 RX overflows/s, `PRACH request late`, zero PUSCH/PDSCH), which is exactly what those numbers
#             predict. A generator that can still produce it is a trap, so it refuses.
#
# usage: bash mk_arm_cfg.sh <sc8> [output-path]
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
    echo "REFUSING arm 'bigframe': recv_frame_size above ~8 KB collapses the B200's receive transport." >&2
    echo "  Measured (wip/uhd_rx_health, RX+TX, 6 s per point): 8192/8200 -> 100% duty, 0 errors;" >&2
    echo "  12288 and 16360 -> 9.8% duty with every block an overflow.  It was flown once as p36 and the UE" >&2
    echo "  could not attach. See dev doc 6.55 (3). Use the ring depth (num_recv_frames) if more margin is" >&2
    echo "  wanted - 512 and 1024 frames measured healthy." >&2
    exit 2
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
