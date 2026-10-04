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
# It prints, per leg, the four things that decide comparability, and then PASS/FAIL for the pair. FAIL does not
# mean the flight was wasted - it means do not quote a carrier ratio from it.
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
  printf "%-6s %-34s %-12s recv=%-9s hops=%-8s B/hop=%s\n" "$leg" "${gaps:-?}" "${dur:-?}" "${recv:-?}" "${hops:-?}" "$pdu"
}

echo "leg    radio continuity                   duration     receives        hops        payload"
report "$1" || exit 2
report "$2" || exit 2
echo
echo "PASS requires: both OK, durations within ~20%, and B/hop within 10%."
echo "Anything else: the pair cannot carry a carrier ratio - re-fly rather than explain."
