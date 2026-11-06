#!/bin/bash
# Turn ONE trained snapshot into a bundle the arm can fly: the full-band model plus its narrow-band buckets.
#
# WHY THIS IS A SCRIPT AND NOT A PARAGRAPH OF INSTRUCTIONS. The narrow buckets are the difference between an arm that
# cannot meet a slot and one that can: measured on 2026-11-06, the same c128 weights cost 2824 us at 612 subcarriers but
# 266 us at 108, and the c32 weights 387 against 137. The runtime picks the smallest bucket that covers the allocation
# plus four PRB of context, so a five-PRB reception runs the narrow one. The trainer has exported buckets for a while and
# the arm has looked for them for longer, but nothing put them on disk in the layout the arm expects -- the bundle carried
# no buckets/ directory, every reception ran the full band, and the flight that lost 39 percent of its uplink blocks lost
# them to latency that this directory removes. The layout is therefore worth encoding once, with the traps named.
#
# WHAT IT WRITES
#   <out>/<name>.mlmodelc                 the widest export (the full-band model)
#   <out>/buckets/base_w<W>.mlmodelc      one per narrower width
#   <out>/depth3_scales.txt               calibration, copied into buckets/ as well
#
# THE TRAP: the arm probes <bundle-dir>/buckets/base_w{108,180,252,324,396,468,540,612}.mlmodelc BY NAME and simply
# counts the ones that do not load, so a misnamed directory is silent -- it flies at full band and looks merely slow.
# After a run, the arm's own line says what bound: "narrow-band buckets bound: ..." ; a run without it did not find them.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
PY=${PY:-$HERE/../../../ai_train/.venv/bin/python}
[ -x "$PY" ] || PY=${PY:-python3}
REPO=$(cd "$HERE/../../../../.." && pwd)
PROBE=${PROBE:-$REPO/build/lib/phy/upper/signal_processors/channel_estimator/metal/coreml_latency_probe}

KERAS=""
NAME=""
C_HID=32
BLOCKS=18
WIDTHS=108,180,252,324,396,468,540,612
OUT=""
CALIB=""
MEASURE=0
SNR="-0.18,34.70"

usage() {
  cat >&2 <<EOF
usage: make_bucket_bundle.sh <snapshot.keras> --name <canonical-name> [options]

  --name NAME         canonical name (model_naming_convention.md); becomes <out>/NAME.mlmodelc
  --c-hid N           trunk width (default 32)
  --blocks N          block count (default 18)
  --widths a,b,c      bucket widths (default 108,180,252,324,396,468,540,612)
  --out DIR           output directory (default \$HOME/ai_depth3/gate_ai_models/bundles/NAME)
  --calib DIR         monitor dataset for calibrate_depth3.py (enables the scales file)
  --snr-range a,b     export range (default $SNR)
  --measure           after compiling, print the ANE latency of each width with $PROBE
EOF
  exit 2
}

while [ $# -gt 0 ]; do
  case "$1" in
    --name) NAME=$2; shift 2;;
    --c-hid) C_HID=$2; shift 2;;
    --blocks) BLOCKS=$2; shift 2;;
    --widths) WIDTHS=$2; shift 2;;
    --out) OUT=$2; shift 2;;
    --calib) CALIB=$2; shift 2;;
    --snr-range) SNR=$2; shift 2;;
    --measure) MEASURE=1; shift;;
    -h|--help) usage;;
    *) KERAS=$1; shift;;
  esac
done
[ -n "$KERAS" ] && [ -n "$NAME" ] || usage
[ -f "$KERAS" ] || { echo "no such snapshot: $KERAS" >&2; exit 1; }
OUT=${OUT:-$HOME/ai_depth3/gate_ai_models/bundles/$NAME}
mkdir -p "$OUT/buckets"

echo "=== $NAME from $KERAS  (c_hid $C_HID, $BLOCKS blocks, widths $WIDTHS)"
"$PY" -u "$HERE/train_depth3.py" --export-only "$KERAS" \
  --export-probe "$OUT/$NAME.mlpackage" --export-buckets "$WIDTHS" \
  --modcond --layout spatial --modulation QPSK --c-hid "$C_HID" --blocks "$BLOCKS" \
  --snr-range="$SNR" 2>&1 | grep -aE "wrote the fp16|bucket width" | tail -"$(echo "$WIDTHS" | tr ',' '\n' | wc -l | tr -d ' ')"

echo "=== compiling"
widest=$(echo "$WIDTHS" | tr ',' '\n' | sort -n | tail -1)
for w in $(echo "$WIDTHS" | tr ',' ' '); do
  pkg="$OUT/$NAME""_w$w.mlpackage"
  [ -d "$pkg" ] || continue
  xcrun coremlcompiler compile "$pkg" "$OUT" >/dev/null
  if [ "$w" = "$widest" ]; then mv "$OUT/$NAME""_w$w.mlmodelc" "$OUT/$NAME.mlmodelc"
  else mv "$OUT/$NAME""_w$w.mlmodelc" "$OUT/buckets/base_w$w.mlmodelc"; fi
done

if [ -n "$CALIB" ]; then
  echo "=== calibrating"
  "$PY" "$HERE/calibrate_depth3.py" --model "$KERAS" --data "$CALIB" \
    --modulation QPSK --cond --layout spatial --samples 60 --write-scales "$OUT" \
    2>&1 | grep -a "chosen" | tail -1
  cp "$OUT/depth3_scales.txt" "$OUT/buckets/depth3_scales.txt"
fi

echo "=== layout"
ls -d "$OUT/$NAME.mlmodelc" "$OUT"/buckets/base_w*.mlmodelc 2>/dev/null | sed "s|$OUT/|  |"

if [ "$MEASURE" = "1" ] && [ -x "$PROBE" ]; then
  echo "=== ANE latency (cpu_ne)"
  for w in $(echo "$WIDTHS" | tr ',' ' '); do
    m="$OUT/buckets/base_w$w.mlmodelc"; [ -d "$m" ] || m="$OUT/$NAME.mlmodelc"
    printf '  w%-4s ' "$w"
    OCUDU_DEPTH3_COMPUTE_UNITS=cpu_ne "$PROBE" --model "$m" --iterations 15 --warmup 5 2>&1 |
      grep -a predict | sed 's/.*p50=\([0-9.]*\)us.*/p50=\1 us/'
  done
fi

cat <<EOF

=== NEXT
  fly it:  gnb -c <config> --expert_phy.phy_pipeline npu \\
                --expert_phy.pusch_receiver_model $OUT/$NAME.mlmodelc
  confirm: the arm's log says "narrow-band buckets bound: ..."; without that line it is flying at full band.
EOF
