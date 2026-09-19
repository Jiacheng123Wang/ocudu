#!/usr/bin/env bash
# The on-air criteria of one leg, out of the two files a leg leaves behind.
#
# A leg writes TWO streams and the verdict needs both:
#   <prefix>          the ocudulog file (--log.filename): crc=OK/KO, "Real-time failure in RF", the ^C series
#   <prefix>.stderr   what the probes print with fprintf(stderr): [metal_stats], [ul_gpu_lane],
#                     [mmse_time_sum], [ul_host], [ul_rx], and the contract table
# run_air_leg.sh tees stderr into the second one; a leg run by hand into a terminal has to be captured
# with `2> >(tee …)` or the counters are lost with the terminal.
#
# usage: bash air_leg_report.sh <log-prefix> [<log-prefix-2> …]
set -u
[ $# -ge 1 ] || { echo "usage: $0 <log-prefix> [<log-prefix-2> …]" >&2; exit 2; }

field() { # field <file> <regex>  -> the value after "=", or "?"
  local v
  v=$( { [ -f "$1" ] && grep -oE "$2" "$1" | head -1 | sed -E 's/.*=//'; } 2>/dev/null )
  echo "${v:-?}"
}

report() {
  local p=$1 log=$1 err=$1.stderr
  # Either file may be handed over: a prefix, the ocudulog file, or the stderr capture.
  case "$1" in
    *.stderr) log=${1%.stderr}; err=$1 ;;
    *)        [ -f "$err" ] || err=$1 ;;
  esac
  echo "================ $(basename "$p")"
  [ -f "$log" ] || { echo "  MISSING ocudulog file: $log"; }
  [ -f "$err" ] || { echo "  MISSING stderr capture: $err  (counters + contract unavailable)"; }

  local ok ko slots rtf
  # grep -c prints 0 and still exits 1 when nothing matches: the count is the value either way.
  ok=$( { grep -c "crc=OK" "$log"; } 2>/dev/null || true); ok=${ok:-0}
  ko=$( { grep -c "crc=KO" "$log"; } 2>/dev/null || true); ko=${ko:-0}
  rtf=$( { grep -c "Real-time failure in RF" "$log"; } 2>/dev/null || true); rtf=${rtf:-0}
  slots=$(field "$err" '\[ul_rx\] blocks=[0-9]+')
  echo "  crc OK/KO            : $ok / $ko"
  if [ "$slots" != "?" ] && [ "$slots" != "0" ]; then
    echo "  Real-time failures   : $rtf / $slots slots = $(awk -v a="$rtf" -v b="$slots" 'BEGIN{printf "%.5f%%", 100*a/b}')   (budget 0.0116%)"
  else
    echo "  Real-time failures   : $rtf / (no [ul_rx] blocks line)"
  fi
  echo "  contract             : $(grep -E '^\[phy_pipeline\] contract' "$err" 2>/dev/null | tail -1 | sed 's/^\[phy_pipeline\] *//')"
  # cut at 120, not 70: device_sigma2 (and its siblings) sit at the END of the line, and the S-7g-20
  # leg's key evidence - that the device produced the ratio on every hop - was being truncated away.
  echo "  mmse_ce              : $(grep -E '^\[metal_stats\] mmse_ce commits' "$err" 2>/dev/null | sed 's/.*mmse_ce //')"
  echo "  burst                : $(grep -E '^\[metal_stats\] burst commits' "$err" 2>/dev/null | sed 's/.*burst //')"
  echo "  dft commits / waits  : $(grep -E '^\[metal_stats\] dft commits' "$err" 2>/dev/null | grep -oE 'commits=[0-9]+ transforms=[0-9]+ waits=[0-9]+' | tail -1)"
  echo "  grid readers (host)  : $(grep -E '^\[metal_stats\] (pusch_demod ch_est|equalizer ch_re)' "$err" 2>/dev/null | sed 's/^\[metal_stats\] *//' | tr '\n' ' ')"
  echo "  front-end fence      : $(grep -E '^\[metal_stats\] front_end fence' "$err" 2>/dev/null | sed 's/^\[metal_stats\] *//')"
  echo "  lane fence (CE->lane): $(grep -E '^\[metal_stats\] lane fence' "$err" 2>/dev/null | sed 's/^\[metal_stats\] *//')"
  echo "  lane order knob      : OCUDU_CE_LANE_ORDER=$(grep -m1 -oE 'OCUDU_CE_LANE_ORDER=[a-z_]+' "$err" 2>/dev/null | head -1 | sed 's/.*=//' || echo '(default event)')"
  echo "  lanes / cbs per lane : $(grep -E '^\[ul_gpu_lane\] lanes' "$err" 2>/dev/null | sed 's/^\[ul_gpu_lane\] *//')"
  for m in residency busy gap; do
    echo "  lane $m$(printf '%*s' $((10 - ${#m})) '') : $(grep -E "^\[ul_gpu_lane\] $m samples" "$err" 2>/dev/null | sed 's/^\[ul_gpu_lane\] *//')"
  done
  # The host's share of that gap (S-7g-20 instrumentation, see ocudu_metal_lane_clock.h). Printed
  # right under the gap it belongs to: the gap alone has two possible owners, and which one it is
  # decides what to optimize next. The OTHER host leg - how long the slot waited before the
  # estimator's stage began - is the [ul_channel_estimation] phase above, which is slot-paired.
  #
  # \note These two lines do NOT decompose the gap, however much they read like it: 'entry->cb' ends
  # at the extraction's COMMIT while the residency (hence the gap) starts at that command buffer's
  # GPUStartTime, so the host leg sits outside the window it looks like it belongs to - and
  # 'cb->start' is the extraction's start against the lane's earliest start, which is the extraction
  # by construction, so it reads 0 whatever happens. The gap is decomposed by the four 'hole' /
  # 'host:' lines below; see 48.194(g-vicies).
  echo "  gap: entry->cb (host): $(grep -E '^\[ul_gpu_lane\] gap: stage entry' "$err" 2>/dev/null | sed 's/^\[ul_gpu_lane\] *//')"
  echo "  gap: cb->start (queue): $(grep -E '^\[ul_gpu_lane\] gap: commit -> first' "$err" 2>/dev/null | sed 's/^\[ul_gpu_lane\] *//')"
  # The same distance for the lane's other two command buffers (see the probe's close_lane()): read
  # together with the line above, they separate "the device was busy when the buffer arrived" from
  # "this one buffer was handed over late". The extraction one was identically 0.0 until it was
  # repaired - it differenced the extraction's start against the lane's own earliest start.
  echo "  queue: wt commit->start: $(grep -E '^\[ul_gpu_lane\] queue: weights' "$err" 2>/dev/null | sed 's/^\[ul_gpu_lane\] *//')"
  echo "  queue: burst commit->start: $(grep -E '^\[ul_gpu_lane\] queue: burst' "$err" 2>/dev/null | sed 's/^\[ul_gpu_lane\] *//')"
  # The gap, actually split by where it sits, with the host's own reading of each transition beside
  # it. hole(extraction->weights) + hole(weights->burst) == gap whenever the lane holds one command
  # buffer per stage; the host pair says whether the host had handed that command buffer over by then.
  echo "  hole: ce->wt (device) : $(grep -E '^\[ul_gpu_lane\] gap: extraction end' "$err" 2>/dev/null | sed 's/^\[ul_gpu_lane\] *//')"
  echo "  hole: wt->burst (dev) : $(grep -E '^\[ul_gpu_lane\] gap: weights end' "$err" 2>/dev/null | sed 's/^\[ul_gpu_lane\] *//')"
  echo "  host: ce->wt commit   : $(grep -E '^\[ul_gpu_lane\] host: extraction commit' "$err" 2>/dev/null | sed 's/^\[ul_gpu_lane\] *//')"
  echo "  host: wt->burst commit: $(grep -E '^\[ul_gpu_lane\] host: weights commit' "$err" 2>/dev/null | sed 's/^\[ul_gpu_lane\] *//')"
  echo "  gpu_wait / defer_wait: $(grep -E '^\[mmse_time_sum\]' "$err" 2>/dev/null | grep -oE 'gpu_wait=[0-9.]+us' | tail -1) / $(grep -E '^\[mmse_time_sum\]' "$err" 2>/dev/null | grep -oE 'defer_wait=[0-9.]+us' | tail -1)"
  echo "  red lines            : $(grep -E '^\[ul_host\]' "$err" 2>/dev/null | tail -1 | sed 's/^\[ul_host\] *//')"
  echo "  wrap / zero-copy     : $(grep -E '^\[metal_stats\] wrap' "$err" 2>/dev/null | sed 's/^\[metal_stats\] *//')"
  echo "  boot line            : $(grep -m1 -oE 'commit [0-9a-f]{10}' "$log" 2>/dev/null)  / OCUDU_DFT_OPEN_BLOCK=$(grep -m1 -oE 'OCUDU_DFT_OPEN_BLOCK=[0-9]+' "$err" 2>/dev/null | head -1 | sed 's/.*=//' || echo '(default on)')  / OCUDU_UL_RX_SYMBOLS=$(grep -m1 -oE 'OCUDU_UL_RX_SYMBOLS=[0-9]+' "$err" 2>/dev/null | head -1 | sed 's/.*=//' || echo '(unset)')"
}
for p in "$@"; do report "$p"; done
