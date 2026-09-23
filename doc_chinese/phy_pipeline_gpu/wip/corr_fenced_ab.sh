#!/usr/bin/env bash
# The air A/B for 5.9.88/5.9.89's fix: does paying one extra command buffer per hop on the channel
# estimator actually cost anything on the air?
#
# WHY THE OFFLINE NUMBER DOES NOT DECIDE IT. The unit test reports mean total ~215us a hop, and the
# extra submission measured ~50us of that - 28%. The AIR reports mean total 16.6us a hop (s60-defer),
# with defer_wait at a median of 840us: the channel estimator's own work is small next to the lane's
# deferred wait. So the same absolute cost is either 3x the CE stage or completely hidden inside a wait
# that is already 900us long, and only a leg can say which.
#
# COVERAGE (checked before sending anyone to run this): the fenced fix is applied on BOTH routes - the
# non-merged one (run_engine_blocks()) and the merged one, where the air interface spends 74% of its
# hops (std_corr_prefix plus the edge group's edge_corr). An earlier revision covered only the
# non-merged route, which would have made this A/B understate both the benefit and the price. The
# evidence that the merged builds are armed is the lane fence signals counter, 12 to 3102.
#
# WHAT TO READ (in this order):
#   1. the contract block - the leg's verdict, and the first thing to look at (leg_report.sh prints it);
#   2. [mmse_time_sum] - mean total / submit / gpu_path, and the defer_wait distribution;
#   3. [ul_pipeline] stale= and the lane span - whether the added submission spilled into the slot;
#   4. real-time failures and crc, from the leg's own log.
#
# usage:
#   # 1) run the two legs (needs the radio and the phone; the agent cannot: sudo wants a password)
#   sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu s61-fenced-off
#   sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu s61-fenced-on OCUDU_CE_CORR_FENCED=1
#   # 2) compare them
#   bash doc_chinese/phy_pipeline_gpu/wip/corr_fenced_ab.sh s61-fenced-off s61-fenced-on
set -u

if [[ $# -ne 2 ]]; then
  echo "usage: bash corr_fenced_ab.sh <label-default> <label-fenced>" >&2
  echo "(the two labels must already have been run; see the header for the two run_leg.sh commands)" >&2
  exit 2
fi

cd "$(dirname "$0")/../../.." || exit 1
LOGS=doc_chinese/phy_pipeline_gpu/wip/logs

newest() { # the newest log whose name carries this label
  ls -t "$LOGS"/*_"$1"_*.log.stderr 2>/dev/null | head -1
}

A=$(newest "$1")
B=$(newest "$2")
if [[ -z $A || -z $B ]]; then
  echo "missing a leg: '$1' -> '${A:-<none>}', '$2' -> '${B:-<none>}'" >&2
  echo "logs looked for under $LOGS" >&2
  exit 1
fi

show() { # $1 = label, $2 = log
  echo "======================================================================"
  echo "== $1   ($2)"
  echo "======================================================================"
  echo "-- verdict (leg_report.sh) --"
  bash doc_chinese/phy_pipeline_gpu/wip/leg_report.sh "$2" 2>&1 | sed 's/^/   /'
  echo "-- channel estimator hop time --"
  grep -h "mmse_time_sum" "$2" 2>/dev/null | tail -2 | sed 's/^/   /'
  echo "-- pipeline / rx pool --"
  grep -hE "^\[ul_pipeline\]|^\[ul_rx_pool\]" "$2" 2>/dev/null | tail -3 | sed 's/^/   /'
  echo "-- lane structure (the extra submission shows up here, whatever the timing does) --"
  grep -hE "^\[ul_gpu_lane\] lanes=|^\[ul_gpu_lane\] residency" "$2" 2>/dev/null | tail -2 | sed 's/^/   /'
  echo "-- real-time failures --"
  grep -hcE "real.?time|RT failure" "$2" 2>/dev/null | sed 's/^/   lines mentioning real-time failures: /'
}

show "default ($1)" "$A"
show "fenced ($2)"  "$B"

echo
echo "======================================================================"
echo "== the two numbers the decision turns on =="
echo "======================================================================"
for pair in "default:$A" "fenced:$B"; do
  lbl=${pair%%:*}
  f=${pair#*:}
  t=$(grep -ho "mean total=[0-9.]*us" "$f" 2>/dev/null | tail -1)
  s=$(grep -ho "submit=[0-9.]*us" "$f" 2>/dev/null | tail -1)
  dw=$(grep -ho "defer_wait distribution: samples=[0-9]* mean=[0-9.]*us median=[0-9.]*us" "$f" 2>/dev/null | tail -1)
  met=$(grep -ho "contract MET ([0-9]* of [0-9]*[^)]*)\|contract NOT MET: [^(]*" "$f" 2>/dev/null | tail -1)
  printf '   %-8s %-22s %-14s %s\n' "$lbl" "$t" "$s" "$met"
  printf '            %s\n' "$dw"
done
