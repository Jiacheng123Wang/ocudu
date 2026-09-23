#!/usr/bin/env bash
# MMSE landmine (section 5.9.84): reproduce the two states and the instrument line-count split.
#
# The unit test's Test 9 is non-deterministic on the default path (device correlation build): about a
# quarter of the runs report a noise-variance drift far above the 1.5 limit. Section 5.9.84's claim is
# that the failing runs take ANOTHER PATH, and that the readable signature is the number of
# "[edge_check] geometry:" lines - 715 when it passes, 644 when it fails - not the DIFFERENT verdict,
# which prints in both states.
#
# This script runs the binary with both check knobs armed and records, per run: the verdict, the worst
# drift and the shape that carried it, and the check-line census. Use it to (a) confirm the split and
# (b) read a knob's effect on it.
#
# Usage:
#   bash doc_chinese/phy_pipeline_gpu/wip/mmse_landmine.sh [runs] [env assignments...]
#   bash .../mmse_landmine.sh 12
#   bash .../mmse_landmine.sh 12 OCUDU_CE_EDGE_FUSE=0
set -u

RUNS=${1:-12}
shift || true

cd "$(dirname "$0")/../../.." || exit 1
BIN=build/lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_unit_test
[[ -x $BIN ]] || { echo "missing $BIN (lib/ is EXCLUDE_FROM_ALL: build the target by name)" >&2; exit 1; }

OUT=$(mktemp -d)
echo "runs=$RUNS knobs: OCUDU_CE_CORR_CHECK=1 OCUDU_CE_EDGE_CHECK=1 $*"

for ((i = 1; i <= RUNS; ++i)); do
  env OCUDU_CE_CORR_CHECK=1 OCUDU_CE_EDGE_CHECK=1 "$@" "$BIN" >"$OUT/run$i.txt" 2>&1
  rc=$?
  python3 - "$OUT/run$i.txt" "$rc" "$i" <<'PY'
import re, sys
path, rc, i = sys.argv[1], int(sys.argv[2]), sys.argv[3]
txt = open(path, errors="replace").read()
shape = "?"
worst = 0.0
bad = "-"
for line in txt.splitlines():
    m = re.match(r"Test 9 \[([^\]]+)\]: level consistency", line)
    if m:
        shape = m.group(1)
    m = re.search(r"nv/l\^2 cpu [0-9.]+ mmse ([0-9.]+)", line)
    if m:
        v = float(m.group(1))
        if v > worst:
            worst, bad = v, shape
geo = txt.count("[edge_check] geometry:")
corr = txt.count("[corr_check] group=")
ck = txt.count("[edge_check] checked A=")
slot = txt.count("[edge_check] R slot:")
print(f"run {i:>3} rc={rc} {'FAIL' if rc else 'pass'} worst_mmse_drift={worst:9.3f} "
      f"on={bad:<16} corr={corr:>5} geo={geo:>5} checked={ck:>5} rslot={slot:>5}")
PY
done
rm -rf "$OUT"
