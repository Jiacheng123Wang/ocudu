#!/usr/bin/env bash
# Build a leg-config ARM from a TEMPLATE, moving exactly the variable under test, and print the diff.
#
# WHY THIS EXISTS. An arm is evidence about ONE variable, so the arm's config must differ from the config
# it is measured against by exactly the variable under test - and that has to be checkable, not asserted. A
# hand-copied config file drifts silently the moment the source config changes (and the leg would then read
# as a clean A/B of one variable while it moved several). Generating it and PRINTING THE DIFF puts the
# variable in the leg's own transcript: `run_leg.sh` copies the config into the leg log, and this script's
# output shows what was moved.
#
# Two families of arms:
#
#   TRANSPORT arms (dev doc 6.54 (C), as the bench probe left them):
#     sc8       otw_format: sc8        - HALF the wire rate per sample (8-bit I/Q instead of 12). It changes the
#                                        waveform, so it would be judged on the TRANSPORT readings only - and the
#                                        probe has since answered its question (the wire is not the constraint),
#                                        so it is offered, not recommended.
#     bigframe  RETIRED - REFUSED. `recv_frame_size` above ~8 KB collapses the B200's receive transport on this
#               host: measured with wip/uhd_rx_health, 6 s each, RX+TX, delivery args otherwise -
#                 8192 / 8200 -> 100% of the time streamed, 0 radio errors
#                 12288       ->  9.8%, every block `overflow`
#                 16360       ->  9.8%, every block `overflow`   (UHD clamps a request of 16384 to 16360)
#               It was flown once as `p36-n78-bigframe` before it was measured: the UE could not attach at all
#               (628 RX overflows/s, `PRACH request late`, zero PUSCH/PDSCH), which is exactly what those numbers
#               predict. A generator that can still produce it is a trap, so it refuses.
#
#   UL-THROUGHPUT arms (2026-09-27, the "5 MHz beats 20 MHz" finding): these move one key of
#   `cell_cfg.pusch`, and they read their source from the A/B TEMPLATES, which carry the knob lines
#   COMMENTED OUT (one line each, so "uncomment one" is a genuine one-line change):
#     rv            rv_sequence: [0, 2, 3, 1]     UL HARQ redundancy-version cycling (default [0]: every
#                                                 retransmission repeats rv=0, so combining gets no RV gain)
#     mcs19         max_ue_mcs: 19                cap below 256QAM (default 28). Motivated by the measurement:
#                                                 256QAM delivered 0.87 bit/RE against 64QAM's 2.89, with
#                                                 80.4% of the 256QAM transmissions failing CRC (90% in the
#                                                 5.5-6.0 bit/RE band) at a 19 dB median SINR.
#     qam64         mcs_table: qam64              the other way to cap (whole table)
#     p0up          p0_nominal_with_grant: -70    raise the UL power-control target (default -76 dBm) to spend
#                                                 the 11-17 dB of PHR the UE was measured to have in hand.
#     p0up_rv_mcs19 the three above together       only after the single-variable arms have shown their own effect
#
#   The templates: configs/gnb_rf_b200_tdd_n78_20mhz_ul_ab.yml  (n78, the cell under test)
#                  configs/gnb_rf_b200_fdd_n1_5mhz_bridge_ul_ab.yml (n1, the matched control - mirror only the
#                  band-independent arms: rv / mcs19 / qam64; p0up is band-specific by nature)
#   Point ARM_SRC at the template you mean; the default is the n78 one.
#
# usage: bash mk_arm_cfg.sh <arm> [output-path]
#        ARM_SRC=<template.yml> bash mk_arm_cfg.sh <arm>          # e.g. the n1 mirror
#        default output: doc_chinese/work_tmp/arm_<name>.yml   (git-ignored, like the other dev products)
set -eu

ARM=${1:?arm name: sc8, rv, mcs19, qam64, p0up, p0up_rv_mcs19}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
# WHERE THE ARM'S SOURCE IS. The transport arms move a line of the DELIVERY config; the UL arms move one of
# the commented knob lines of the A/B TEMPLATE (whose whole point is that they ship commented). Defaulting
# them all to the delivery config produced "0 changed lines" for every UL arm on the first run - the guard
# below caught it, which is why the guard exists. ARM_SRC overrides either default (that is how the n1
# mirror is generated from the n1 template).
case "$ARM" in
  rv|mcs19|qam64|p0up|p0up_rv_mcs19) DEF_SRC="$ROOT/configs/gnb_rf_b200_tdd_n78_20mhz_ul_ab.yml" ;;
  *)                                 DEF_SRC="$ROOT/configs/gnb_rf_b200_tdd_n78_20mhz.yml" ;;
esac
SRC=${ARM_SRC:-$DEF_SRC}
OUT=${2:-$ROOT/doc_chinese/work_tmp/arm_${ARM}.yml}

[ -f "$SRC" ] || { echo "missing source config: $SRC" >&2; exit 2; }
mkdir -p "$(dirname "$OUT")"

# Uncomment ONE knob line of the template: the templates write each arm's key as `#    key: value  # note`,
# so stripping the leading `#` yields a correctly indented (4 spaces, under `pusch:` at 2) live key.
uncomment() {   # <ere matching the knob text after "#    ">
  sed -E "s/^#(    $1)/\1/" "$SRC" > "$OUT"
}

WANT=()          # the strings the arm's output must carry (checked below)
EXPECT_CHANGED=2 # one removed + one added; a multi-line arm raises it

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
    WANT=("otw_format: sc8")
    ;;
  rv)
    uncomment 'rv_sequence: \[0, 2, 3, 1\]'
    WANT=("rv_sequence: [0, 2, 3, 1]")
    ;;
  mcs19)
    uncomment 'max_ue_mcs: 19'
    WANT=("max_ue_mcs: 19")
    ;;
  qam64)
    uncomment 'mcs_table: qam64'
    WANT=("mcs_table: qam64")
    ;;
  p0up)
    uncomment 'p0_nominal_with_grant: -70'
    WANT=("p0_nominal_with_grant: -70")
    ;;
  p0up_rv_mcs19)
    sed -E \
      -e 's/^#(    p0_nominal_with_grant: -70)/\1/' \
      -e 's/^#(    rv_sequence: \[0, 2, 3, 1\])/\1/' \
      -e 's/^#(    max_ue_mcs: 19)/\1/' "$SRC" > "$OUT"
    WANT=("p0_nominal_with_grant: -70" "rv_sequence: [0, 2, 3, 1]" "max_ue_mcs: 19")
    EXPECT_CHANGED=6
    ;;
  *)
    echo "unknown arm '$ARM': expected sc8 | rv | mcs19 | qam64 | p0up | p0up_rv_mcs19" >&2
    exit 2
    ;;
esac

DIFF=$(diff -u "$SRC" "$OUT" || true)
N_CHANGED=$(printf '%s\n' "$DIFF" | grep -cE '^[+-][^+-]' || true)
if [ "$N_CHANGED" != "$EXPECT_CHANGED" ]; then
  echo "REFUSING: the arm is not the expected one-line change ($N_CHANGED changed lines, expected $EXPECT_CHANGED)." >&2
  printf '%s\n' "$DIFF" >&2
  exit 2
fi
for w in "${WANT[@]}"; do
  if ! grep -qF -- "$w" "$OUT"; then
    echo "REFUSING: the arm does not carry '$w' - the substitution did not apply." >&2
    echo "  source: $SRC" >&2
    grep -qE '^#    ' "$SRC" || echo "  (this source carries NO commented knob line: the UL arms need one of the" >&2
    grep -qE '^#    ' "$SRC" || echo "   A/B templates as ARM_SRC, e.g. configs/gnb_rf_b200_tdd_n78_20mhz_ul_ab.yml)" >&2
    exit 2
  fi
done

echo "# arm '$ARM' generated from $(basename "$SRC") - the variable it moves:"
printf '%s\n' "$DIFF" | grep -E '^[+-][^+-]' | sed 's/^/  /'
echo "# file: $OUT"
echo "# fly it with:  LEG_CONFIG=$OUT"
echo "# and judge it on: grants/s, retransmission share, BLER per modulation, effective bit/RE, iperf3 rx Mbit/s"
