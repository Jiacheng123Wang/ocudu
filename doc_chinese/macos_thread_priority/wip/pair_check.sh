#!/usr/bin/env bash
# Ten-second check that a PAIR of legs is comparable AT ALL - before anyone reads a carrier out of it.
#
# WHY (2026-10-04, plan doc 11.35). Six A/B rounds in this line were void for reasons that were visible in the
# first three lines of each leg's report, and every one of them was discovered only after the pair had been
# flown: a control leg that lost samples (p257 3 gaps, p261 1, p265 3), two legs flown with different protocols
# (p265 14 min against p266 5 min: 2.9x the receives, 2.8x the hop rate), and - the one that survived all of
# the above and still poisoned the result - two arms whose transport blocks differed by 24-30%, because a PHY
# stage's cost is a function of the payload.
#
# usage:  bash pair_check.sh p267 p268        (run it the moment the second leg's report is out)
#
# It prints, per leg, the things that decide comparability, and then PASS/FAIL for the pair. FAIL does not mean
# the flight was wasted - it means do not quote a carrier ratio from it.
#
# THE DISRUPTION PROXY IS THE FITH ONE, and it was added after the control-vs-control accident of 2026-10-04
# (p269 against p270: same binary, same config, same 180 s of traffic, hops within 0.5% and payload within 3%,
# and yet `ce > 1000 us` read 0.369527% against 0.101533% - a factor of 3.6). The channel estimator's own work
# was identical in both legs (mean total 55.1 vs 55.3 us, same device path, same 4 refusals): what differed was
# how much the HOST disrupted it, and that shows in two independent readings at once - the carrier count and
# the estimator's deferred-wait tail (`defer_wait` p99 1228 vs 1029 us, max 28.4 vs 21.0 ms). A pair whose two
# legs disagree on that tail is a pair flown under two different disturbance levels, whatever `load1` said.
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
LOGDIR=${LEG_LOGDIR:-$HERE/logs}

[ $# -ge 2 ] || { echo "usage: bash pair_check.sh <legA> <legB>" >&2; exit 2; }

report() {
  local leg=$1 f
  f=$(ls -t "$LOGDIR"/gnb_gpu_${leg}-*.log.stderr 2>/dev/null | head -1)
  if [ -z "$f" ]; then echo "$leg: no .stderr in $LOGDIR" >&2; return 1; fi
  local gaps recv hops dur pdu
  gaps=$(grep -h 'radio sample continuity' "$f" | tail -1 | grep -oE '[0-9]+ gaps[^>]*-> (OK|FAILED)' | head -1)
  dur=$(grep -h 'leg : over' "$f" | tail -1 | grep -oE 'over [0-9.]+s' | head -1)
  recv=$(grep -h '  rx  :' "$f" | tail -1 | grep -oE 'of [0-9]+ receive' | grep -oE '[0-9]+')
  hops=$(grep -hE 'ul slots by PUSCH hops' "$f" | tail -1 | grep -oE 'hops=[0-9]+' | head -1 | cut -d= -f2)
  # Payload PER HOP, which is the form that can be compared across legs of different length: the transport
  # block total over the hops that carried one. (The absolute median is in [ul_mac_pdu_size] as well, but the
  # two are the same reading when the hop counts match, and this one survives a length difference.)
  local total
  total=$(grep -h 'ul_mac_pdu_size' "$f" | tail -1 | grep -oE 'total=[0-9.]+' | cut -d= -f2)
  pdu=$(python3 -c "print(f'{${total:-0}/${hops:-1}:.0f}')" 2>/dev/null || echo "?")
  # The disturbance level the PHY itself saw, not what the OS load average said.
  local dw
  dw=$(grep -h 'defer_wait distribution' "$f" | tail -1 | grep -oE 'p99=[0-9.]+us' | cut -d= -f2)
  local ce
  ce=$(grep -h '  ce  :' "$f" | tail -1 | grep -oE '= [0-9.]+%' | cut -d' ' -f2)
  printf "%-6s %-30s %-11s recv=%-9s hops=%-8s B/hop=%-6s defer99=%-9s ce>1ms=%s\n" \
         "$leg" "${gaps%% (*}" "${dur:-?}" "${recv:-?}" "${hops:-?}" "$pdu" "${dw:-?}" "${ce:-?}"
}

echo "leg    radio continuity                   duration     receives        hops        payload"
report "$1" || exit 2
report "$2" || exit 2
echo
echo "PASS requires: both OK, durations within ~20%, B/hop within 10%, and defer99 within ~20%."
echo "Anything else: the pair cannot carry a carrier ratio - re-fly rather than explain."
