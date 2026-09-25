#!/usr/bin/env bash
# WHAT ONE CHANNEL-ESTIMATOR KERNEL COSTS ON THE GPU (dev doc 3.3): the per-kernel marginal cost, offline.
#
# ---- Why this exists, and why it is a SCRIPT and not a new .mm ------------------------------------
# 6.63/6.64 ended with a budget question: `merged_hop` is ~534 us per hop, the remaining dispatch arms
# are worth ~30-42 us of it, and the rest was written down as "~450 us of real compute + inter-dispatch
# waits" - a number nobody had measured. Deciding WHICH kernel to rewrite needs that split, and the
# engine already carries the honest instrument for it: `mmse_engine_impl::stage_repeat()` dispatches a
# stage N times over the SAME input, so the growth of the lane's GPU window per repetition IS that
# stage's cost, dispatch overhead included, and the published dumps stay byte-identical (the knob's own
# comment records the 235-dump check).
#
# The reading is the command buffer's own GPU time (`[metal_stats] gpu busy (back_end)`), so it is the
# device's number and not the host's; it is the SUM over the run, so the slope between N and 1 divides
# out everything that did not get repeated.
#
# ---- The two questions this answers --------------------------------------------------------------
#   1. per-kernel: how many us does one more dispatch of THIS kernel cost at THIS geometry? (the
#      column the next kernel rewrite has to beat - and the same unit the dispatch arms were measured
#      in, which is what makes the two comparable at all)
#   2. how much of the busy window is EXECUTION at all? `busy` against the observation `window`, and
#      the sum of the per-kernel slopes against the hop's residency, is the split that decides whether
#      the remaining V1 is a kernel problem or a scheduling one.
#
# ---- How to run (offline; it touches the GPU, so NOT while a leg is being flown) ------------------
#   bash doc_chinese/phy_latency/wip/ce_kernel_cost.sh [capture] [N]
#     capture: a basename in doc_chinese/work_tmp/corpus/ (default syn004_4, the 4 PRB hop whose
#              merged batch the legs' geometry resembles: L=54, 1 block, 2 systems, 2 groups)
#     N:       the repetition count (default 33; the engine clamps to 64)
#
# NOTE the corpus hop is ONE device hop per run (the other 16 engine calls of a capture are the CPU
# reference route and are constant across N), which is exactly what makes the slope clean.
set -u
CAP=${1:-syn004_4}
N=${2:-33}
ROOT=/Users/jiachengwang/dev/ocudu
BIN=$ROOT/build/lib/phy/upper/channel_processors/metal/ul_chain_replay
CAPTURE=$ROOT/doc_chinese/work_tmp/corpus/$CAP

[ -x "$BIN" ] || { echo "missing $BIN (build it first: cmake --build build --target ul_chain_replay)" >&2; exit 2; }
[ -f "$CAPTURE.bin" ] || { echo "missing capture $CAPTURE.bin" >&2; exit 2; }

# one run: echoes "<busy_us> <window_us> <device_hops> <busy_commits>"
run() {
  local envs=$1 d busy window hops commits
  d=$(mktemp -d)
  # shellcheck disable=SC2086
  env $envs OCUDU_METAL_GPU_TIME=1 "$BIN" "$CAPTURE" --metal --out "$d/x" >"$d/log" 2>&1
  busy=$(grep -ao "gpu busy (back_end): commits=[0-9]* busy=[0-9.]*us mean=[0-9.]*us window=[0-9.]*us" "$d/log" |
         tail -1 | grep -oE "busy=[0-9.]+" | cut -d= -f2)
  window=$(grep -ao "gpu busy (back_end): .*window=[0-9.]*us" "$d/log" | tail -1 | grep -oE "window=[0-9.]+" | cut -d= -f2)
  commits=$(grep -ao "gpu busy (back_end): commits=[0-9]*" "$d/log" | tail -1 | grep -oE "[0-9]+")
  # the DEVICE hops of this run: the direct route's applies, or the y writes when the knob is off
  hops=$(grep -aoE "(lse_applies|device_y_writes)=[0-9]+" "$d/log" | tail -1 | cut -d= -f2)
  rm -rf "$d"
  [ -n "$busy" ] || { echo "0 0 0 0"; return; }
  echo "$busy $window $hops $commits"
}

read -r BASE_BUSY BASE_WINDOW HOPS BASE_COMMITS <<<"$(run "")"
if [ "$HOPS" = "0" ] || [ -z "$HOPS" ]; then
  echo "FAILED: the run produced no device hop (no busy line or no applies) - not evidence" >&2
  exit 2
fi

printf 'capture=%s  device hops/run=%s  baseline busy(back_end)=%.1fus over %s commit(s), window=%.1fus, N=%s\n' \
       "$CAP" "$HOPS" "$BASE_BUSY" "$BASE_COMMITS" "$BASE_WINDOW" "$N"
printf '%-34s %14s %16s\n' "kernel (knob)" "extra dispatch" "us per dispatch"
printf '%-34s %14s %16s\n' "---------------------------------" "--------------" "----------------"

# label:ENV - the order is the chain's own order (K0-a's pilots, then the matrices, then the weights).
for entry in \
  "mmse_pilots_lse:OCUDU_CE_LSE_REPEAT" \
  "mmse_pilots_apply_cfo:OCUDU_CE_CFO_REPEAT" \
  "mmse_pilots_smooth:OCUDU_CE_SMOOTH_REPEAT" \
  "mmse_pilots_power:OCUDU_CE_POWER_REPEAT" \
  "mmse_pilots_epre:OCUDU_CE_EPRE_REPEAT" \
  "sigma2 chain:OCUDU_CE_SIGMA2_REPEAT" \
  "corr A + R_hp:OCUDU_CE_CORR_REPEAT" \
  "invert K1:OCUDU_CE_INV_REPEAT" \
  "weights K1b:OCUDU_CE_W_REPEAT" \
  "reformat K3:OCUDU_CE_REFORMAT_REPEAT"
do
  label=${entry%%:*}
  knob=${entry##*:}
  read -r busy window hops commits <<<"$(run "$knob=$N")"
  if [ -z "$busy" ] || [ "$busy" = "0" ]; then
    printf '%-34s %14s %16s\n' "$label" "READ FAILED" "-"
    continue
  fi
  slope=$(python3 -c "print(f'{(float('$busy') - float('$BASE_BUSY')) / (($N - 1) * max(1,$HOPS)):.3f}')")
  printf '%-34s %14s %16s\n' "$label" "N=$N" "$slope"
done

echo
echo "read it as: the us in the last column are what ONE more dispatch of that kernel costs THIS"
echo "geometry - dispatch overhead included, which is also the unit the dispatch-elimination arms"
echo "(6.63/6.64: 5-14 us per removed dispatch, paired 10-14) are measured in. Compare the SUM of the"
echo "slopes against the hop's residency (legs: merged_hop ~534us): what is left over is not compute."
