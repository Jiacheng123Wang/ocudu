#!/usr/bin/env bash
# O1 A/B, OFF-LINE AND AT FIXED GEOMETRY: what does merging the two correlation kernels into ONE
# dispatch actually save?
#
# WHY NOT THE AIR LEGS. Legs p146/p147 could not answer it: the two ran at corr rates of 1.402 and 1.533
# per hop (a 9% difference in the very quantity under test), so their ce_weights difference (32.2 vs
# 42.0us) mixes the knob with the geometry. Replay fixes that - it plays the SAME capture every time, so
# the dispatch census is identical by construction and the only variable left is the knob.
#
# WHY NOT `gpu busy` DIRECTLY. It is the UNION of every commit of the run, and dev doc 6.65 measured what
# that costs: slopes of -130us per dispatch, i.e. noise. So each run contributes busy/HOPS - the device
# time per hop - and the reading is the MEDIAN over runs, with the min and max printed beside it so a
# spread that swamps the effect is visible instead of being averaged into a verdict.
#
#   usage: bash doc_chinese/phy_latency/wip/corr_merged_ab.sh [capture] [runs]
set -u
CAP=${1:-syn004_4}
RUNS=${2:-9}
ROOT=/Users/jiachengwang/dev/ocudu
BIN=$ROOT/build/lib/phy/upper/channel_processors/metal/ul_chain_replay
CAPTURE=$ROOT/doc_chinese/work_tmp/corpus/$CAP

[ -x "$BIN" ] || { echo "missing $BIN (cmake --build build --target ul_chain_replay)" >&2; exit 2; }
[ -f "$CAPTURE.bin" ] || { echo "missing capture $CAPTURE.bin" >&2; exit 2; }

# one run -> "<busy_per_hop> <window> <merged_sites> <corr_a_sites>"
run() {
  local envs=$1 d busy hops merged corra window
  d=$(mktemp -d)
  # shellcheck disable=SC2086
  env $envs OCUDU_METAL_GPU_TIME=1 OCUDU_CE_TIME=1 "$BIN" "$CAPTURE" --metal --out "$d/x" >"$d/log" 2>&1
  busy=$(grep -ao "gpu busy (back_end): commits=[0-9]* busy=[0-9.]*us" "$d/log" | tail -1 | grep -oE "busy=[0-9.]+" | cut -d= -f2)
  window=$(grep -ao "gpu busy (back_end): .*window=[0-9.]*us" "$d/log" | tail -1 | grep -oE "window=[0-9.]+" | cut -d= -f2)
  hops=$(grep -aoE "hops_gpu=[0-9]+" "$d/log" | tail -1 | cut -d= -f2)
  # the census this A/B turns on: `merged` is incremented ONLY by the merged dispatch, so 0 vs non-zero
  # is the arm identity, and it is a ratio to corr_a so the geometry cancels.
  read -r merged corra < <(grep -ao "ce_sites .*" "$d/log" | tail -1 |
    sed 's/.*corr_a=\([0-9]*\).*merged=\([0-9]*\).*/\2 \1/')
  rm -rf "$d"
  if [ -z "${busy:-}" ] || [ -z "${hops:-}" ] || [ "${hops:-0}" = "0" ]; then
    echo "0 0 0 0"
    return
  fi
  python3 -c "print(f'{float('$busy')/float('$hops'):.3f} {$window:-0} {${merged:-0}} {${corra:-0}}')"
}

median() { python3 -c "
import sys,statistics
v=[float(x) for x in sys.argv[1:] if float(x)>0]
print(f'{statistics.median(v):.3f}' if v else 'nan')" "$@"; }

echo "capture=$CAP  runs=$RUNS  (busy is DEVICE time per hop; smaller is better)"
printf '%-22s %12s %12s %10s %10s %12s\n' "arm" "busy/hop us" "min" "max" "merged" "corr_a"
for arm in "control:" "merged:OCUDU_CE_CORR_MERGED=1"; do
  label=${arm%%:*}
  envs=${arm##*:}
  vals=()
  merged_seen=0
  corra_seen=0
  for _ in $(seq "$RUNS"); do
    out=$(run "$envs")
    set -- $out
    vals+=("$1")
    merged_seen=$3
    corra_seen=$4
  done
  med=$(median "${vals[@]}")
  min=$(python3 -c "import sys;v=[float(x) for x in sys.argv[1:] if float(x)>0];print(f'{min(v):.3f}' if v else 'nan')" "${vals[@]}")
  max=$(python3 -c "import sys;v=[float(x) for x in sys.argv[1:] if float(x)>0];print(f'{max(v):.3f}' if v else 'nan')" "${vals[@]}")
  printf '%-22s %12s %12s %10s %10s %12s\n' "$label" "$med" "$min" "$max" "$merged_seen" "$corra_seen"
done
