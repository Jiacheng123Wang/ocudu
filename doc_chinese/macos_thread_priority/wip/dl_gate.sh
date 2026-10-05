#!/usr/bin/env bash
# THE STALL GATE + THE DL CRITERION, in one command: is this leg readable at all, and what did its DOWNLINK do?
#
# WHY (dev doc macos_thread_priority 11.92). Two lessons, each paid for with a wasted pair on 2026-10-05:
#
#  1. THE STALL GATE. p298 was flown as the verdict leg for the pool declarations and was not one: it carried a
#     7 ms radio/host delivery stall ([ul_rx_wait] max 7618us, FRONTIER lag 7332us, three radio.rx watchdog
#     events) which produced its ENTIRE tail - pipeline max 1331us and 4 of its samples above 750us. Those bursts
#     are environmental, arrive roughly every ten minutes (see preflight_quiet.sh's header), and their frequency
#     does NOT track how many threads a leg declared: p295 had a 21 ms stall with a SINGLE time-constrained
#     thread, p298 7 ms with six. So a leg carrying one cannot be read for a sub-millisecond effect.
#
#     WHAT THE GATE IS, AND WHAT IT IS NOT. It is not "frontier max < 1 ms": p296/p297 - the pair the uplink
#     verdict was actually taken from - show frontier max 2900/1108us and rx_wait max 2730/1785us, and are still
#     readable, because a long receive wait only damages a slot that CARRIES a judged transport block, and none
#     of theirs did (each has exactly ONE pipeline sample above 750us, at 948/869us). What distinguishes a
#     readable leg is the DL symptom: [dl_tx_slack] hands a transmission over late when the host stalls on a slot
#     that matters, and it counts them. p298: 33 late, and 1.93 % of its transmissions inside 1 ms against the
#     healthy legs' 0.68-0.72 %. So the gate is the DL lateness (and the health), and the frontier/rx_wait maxima
#     are printed as DIAGNOSIS next to it - read them, do not gate on them.
#
#  2. THE RIGHT SIDE. The pool workers are not on the uplink path in the inline shape (samples -> CRC all happen
#     on the lower_phy_rx thread), so [ul_pipeline] cannot judge a pool declaration: it measures a path that never
#     enters the declared threads. The downlink can - [dl_tx_slack] is the margin between a DL transmission's
#     hand-over and its due time.
#
# usage: bash dl_gate.sh <leg> [<leg> ...]     # labels or log paths
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIRS=("$HERE/logs" "$HERE/../../phy_pipeline_gpu/wip/logs")
LATE_BOUND=${LATE_BOUND:-5}   # [dl_tx_slack] AT/BELOW 0 tolerated before a leg is called stalled
STALL_BOUND=${STALL_BOUND:-5000}  # frontier/rx_wait max (us) that separates a stalled leg from a readable one

resolve() {
  local a="$1"
  [ -f "$a" ] && { echo "$a"; return; }
  local hits=""
  for d in "${DIRS[@]}"; do
    hits="$hits $(ls -t "$d"/gnb_*_"$a"*.log.stderr 2>/dev/null)"
  done
  set -- $hits
  [ $# -gt 0 ] && echo "$1"
}

# The health verdicts, once, for the verdict column below.
HEALTH="$(bash "$HERE/ul_health.sh" "$@" 2>&1)"
health_of() { echo "$HEALTH" | awk -v l="$1" '$1==l {print $NF}'; }

fail=0
echo "STALL GATE + DL CRITERION   (late bound: AT/BELOW 0 <= $LATE_BOUND; stall bound: frontier/rx_wait < $STALL_BOUND us)"
printf '%-11s %-6s %-9s %-11s %-11s %-9s %-9s %-9s %-9s %s\n' \
  leg rttc frontier rx_wait "below1ms/1k" "below500" "AT/BELOW0" "wd late" health verdict
for leg in "$@"; do
  f="$(resolve "$leg")"
  if [ -z "$f" ]; then printf '%-11s %s\n' "$leg" "NO LOG FOUND"; fail=1; continue; fi
  frontier=$(grep -a "FRONTIER delivery lag" "$f" | grep -o 'max=[0-9-]*' | head -1 | cut -d= -f2)
  rxwait=$(grep -a '^\[ul_rx_wait\] samples' "$f" | grep -o 'max=[0-9.]*' | cut -d= -f2)
  dl=$(grep -a '^\[dl_tx_slack\]' "$f")
  tx=$(echo "$dl" | grep -o 'transmissions=[0-9]*' | cut -d= -f2)
  b1=$(echo "$dl" | grep -o 'below 1ms=[0-9]*' | cut -d= -f2)
  b5=$(echo "$dl" | grep -o 'below 500us=[0-9]*' | cut -d= -f2)
  late=$(echo "$dl" | grep -o 'AT/BELOW 0=[0-9]*' | cut -d= -f2)
  wd=$(grep -a '^\[ul_watchdog\] OCUDU' "$f" | grep -o 'late max=[0-9.]*' | cut -d= -f2)
  rttc=$(grep -a -c '^\[sched_tc\]' "$f")
  rate=$(awk -v a="${b1:-0}" -v b="${tx:-0}" 'BEGIN { printf (b > 0) ? "%.2f" : "?", (b > 0) ? 1000.0 * a / b : 0 }')
  h="$(health_of "$leg")"
  verdict="PASS"
  # TWO CONDITIONS, because either one alone lets a stalled leg through - measured on 2026-10-05:
  #  * the DL late count alone passed p302 (4 late) while it carried a 12.5 ms delivery stall, and
  #  * the maxima alone would fail p296/p297 (2900/1108 us), which are perfectly readable
  #    (one long receive wait only damages a slot that carries a judged transport block).
  # The 5 ms bound separates every leg flown so far: stalled = p295 (21375), p298 (7332), p302 (12495);
  # readable = the rest, all at or below 2900 us.
  if [ -z "$late" ]; then verdict="NO DL READING"; fail=1
  elif [ "$late" -gt "$LATE_BOUND" ]; then verdict="STALL - re-fly"; fail=1
  elif [ -n "$frontier" ] && [ "$frontier" -ge "$STALL_BOUND" ]; then verdict="STALL(frontier) - re-fly"; fail=1
  elif [ -n "$rxwait" ] && [ "${rxwait%.*}" -ge "$STALL_BOUND" ]; then verdict="STALL(rx_wait) - re-fly"; fail=1
  elif [ "$h" != "CLEAN" ]; then verdict="HEALTH ($h)"; fail=1
  fi
  printf '%-11s %-6s %-9s %-11s %-11s %-8s %-9s %-9s %-9s %s\n' \
    "$leg" "${rttc:-?}" "${frontier:-?}" "${rxwait:-?}" "$rate" "${b5:-?}" "${late:-?}" "${wd:-?}" "$h" "$verdict"
done
echo
echo "$HEALTH" | tail -n $(($# + 3))
echo
echo "READ  rttc          how many threads this leg put on a Mach time constraint (app + ps -M agree: see 11.92)."
echo "      frontier/rx_wait  the delivery stalls, as DIAGNOSIS (they only matter when one lands on a judged slot)."
echo "      below1ms/1k       [dl_tx_slack] transmissions handed over with under 1 ms of margin, per 1000."
echo "      AT/BELOW 0        handed over LATE. Healthy legs: 0-2. Stalled legs: 33 (p298), 21 (p295)."
echo "      => a pool declaration is judged on below1ms/below500/AT-BELOW-0, NOT on [ul_pipeline]."
[ "$fail" -eq 0 ] || echo "VERDICT: at least one leg fails the gate - re-fly before reading the numbers."
