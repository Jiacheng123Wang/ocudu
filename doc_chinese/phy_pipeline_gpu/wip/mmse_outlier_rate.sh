#!/usr/bin/env bash
# MMSE landmine (section 5.9.84): the outlier RATE, as a knob A/B.
#
# The unit test's Test 9 aborts on the first shape whose nv/level^2 drift crosses 1.5, so "how many of
# N runs fail" is a rate, not a verdict - and a 22% baseline needs ~20 runs per arm before a knob's
# effect is readable at all. This runs the binary N times per arm and prints, per arm, the number of
# failing sweeps and the outliers their dumps carry (level, realization index, factor over the median),
# which is the shape of the defect: ONE realization per failing sweep, at a random index.
#
# Usage:
#   bash doc_chinese/phy_pipeline_gpu/wip/mmse_outlier_rate.sh 20
#   bash doc_chinese/phy_pipeline_gpu/wip/mmse_outlier_rate.sh 20 OCUDU_CE_LSE_REPEAT=32
#   bash doc_chinese/phy_pipeline_gpu/wip/mmse_outlier_rate.sh 20 "" OCUDU_CE_LSE_REPEAT=32   # two arms
set -u

RUNS=${1:-20}
shift || true

cd "$(dirname "$0")/../../.." || exit 1
BIN=build/lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_unit_test
[[ -x $BIN ]] || { echo "missing $BIN (lib/ is EXCLUDE_FROM_ALL: build the target by name)" >&2; exit 1; }

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

scan() { # $1 = tag, rest = env assignments
  local tag=$1
  shift
  local fails=0
  for ((i = 1; i <= RUNS; ++i)); do
    env "$@" "$BIN" >"$OUT/$tag-$i.txt" 2>&1 || true
    grep -q "FAILING SWEEP" "$OUT/$tag-$i.txt" && fails=$((fails + 1))
  done
  python3 - "$tag" "$fails" "$RUNS" "$OUT" <<'PY'
import re, sys, glob, statistics
tag, fails, runs, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
rows = []
worst_drift = 0.0
for p in glob.glob(f"{out}/{tag}-*.txt"):
    txt = open(p, errors="replace").read()
    # The sweep's own verdict line carries the drift even when nothing is wrong - that is the number
    # the acceptance gate reads ("twenty consecutive runs at or below 1.1"), so it is collected for
    # every run and not only for the failing ones.
    for m in re.finditer(r"nv/l\^2 cpu [0-9.]+ mmse ([0-9.]+)", txt):
        worst_drift = max(worst_drift, float(m.group(1)))
    if "FAILING SWEEP" not in txt:
        continue
    for line in txt[txt.index("FAILING SWEEP"):].splitlines():
        m = re.match(r"\s*lvl ([\d.e+-]+) mmse\s*:(.*)", line)
        if not m:
            continue
        vals = []
        for tok in m.group(2).split():
            try:
                vals.append(float(tok))
            except ValueError:
                break
        if not vals:
            continue
        med = statistics.median(vals)
        for i, v in enumerate(vals):
            if v > max(3 * med, 2e-2):
                rows.append((float(m.group(1)), i, v / med))
print(f"arm {tag or '(default)':<28} failing_sweeps={fails}/{runs}  outliers={len(rows)}  "
      f"worst_mmse_drift={worst_drift:.3f}")
for lvl, i, f in sorted(rows, key=lambda r: -r[2]):
    print(f"      level={lvl:<9.3e} realization={i:<3} factor={f:8.0f}x")
PY
}

if [[ $# -eq 0 ]]; then
  scan default
else
  first=1
  for arm in "$@"; do
    if [[ $first -eq 1 && -z $arm ]]; then
      scan default
      first=0
      continue
    fi
    # one arm = a space-separated list of KEY=VALUE pairs; the tag is the first key
    scan "$(echo "$arm" | tr ' ' '\n' | head -1 | cut -d= -f1)" $arm
  done
fi
