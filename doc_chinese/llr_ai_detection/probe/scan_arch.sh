#!/usr/bin/env bash
# Architecture -> ANE latency, before paying for a training run.
#
# Every variant is exported with `train_depth3.py --probe-graph` (RANDOM WEIGHTS: a measurement artifact that
# must never receive), compiled, and timed through the real arm in one process (soak, 700 receptions, warm
# p50). The MAC column is the analytic count of the same graph, so the table says how close to the FLOPs the
# ANE actually gets. `blocks=0` is the FLOOR of this plane shape -- entry conv, the four heads and the
# reshape/transpose/pool plumbing, with no trunk at all.
#
# Usage: scan_arch.sh "<c_hid> <blocks>" ...      e.g.  scan_arch.sh "64 18" "48 18" "64 0"
set -u
W=/Users/jiachengwang/dev/ocudu/doc_chinese/llr_ai_detection/work_tmp
P=/Users/jiachengwang/dev/ocudu/lib/phy/upper/ai_signal_processors/depth3/ai_train
PY=$HOME/ai_ce_work/venv/bin/python
G=$W/data/probe_graphs
CAP=$W/corpus/aillr_256qam_ai_s1/C_7_17925
mkdir -p "$G"

printf "%-12s %8s %8s %10s %10s\n" variant c_hid blocks MACs "fwd p50"
for spec in "$@"; do
  c=${spec%% *}
  b=${spec##* }
  tag="arch_c${c}_b${b}"
  if [ ! -d "$G/$tag.mlmodelc" ]; then
    $PY "$P/train_depth3.py" --probe-graph "$G/$tag.mlpackage" --blocks "$b" --c-hid "$c" \
      --modcond --layout spatial --modulation 256QAM > "$G/$tag.export.log" 2>&1 || { echo "$tag: export FAILED"; continue; }
    xcrun coremlcompiler compile "$G/$tag.mlpackage" "$G" >> "$G/$tag.export.log" 2>&1 || { echo "$tag: compile FAILED"; continue; }
  fi
  macs=$($PY - "$c" "$b" <<'PYEOF'
import sys
c, b, width, symbols, cin = int(sys.argv[1]), int(sys.argv[2]), 612, 14, 16
total = width * symbols * (cin * c + b * (3 * c + c * c) + 8 * c + c + 2 * c) + c * 1
print(f"{total/1e6:.0f}M")
PYEOF
)
  # Three runs and the MINIMUM, because this machine has a slow mode in which the host-side segments are up to
  # 8x slower (design section 19.168) and that mode only ever INFLATES a reading. The minimum is therefore the
  # best estimate of the fast state, which is what an architecture comparison needs. Placement is not a
  # confounder: every variant here was shown to be on the ANE by comparing its net values under `all` and
  # `cpu_ne` (identical for all nine).
  best=""
  planes=""
  for run in 1 2 3; do
    out=$(bash "$W/probe/soak_timing.sh" "$CAP" 700 "$G/$tag.mlmodelc" "scan_${tag}_$run" 2>&1)
    line=$(echo "$out" | grep '500 forward' | tail -1)
    fwd=$(echo "$line" | grep -o 'forward p50 [0-9]*' | awk '{print $3}')
    planes="$planes $(echo "$line" | grep -o 'plane p50 [0-9]*' | awk '{print $3}')"
    if [ -n "${fwd:-}" ] && { [ -z "$best" ] || [ "$fwd" -lt "$best" ]; }; then best=$fwd; fi
  done
  printf "%-12s %8s %8s %10s %10s  (plane%s)\n" "$tag" "$c" "$b" "$macs" "${best:-FAIL}" "$planes"
done
