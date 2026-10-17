#!/usr/bin/env bash
# A regression test for the instrument, because the instrument has been wrong three times before it was right.
#
# What it checks: `flight_verdict.py` run on the r8abba batch must reproduce the numbers the flight record holds --
# the three adjacent-leg comparability differences (1.04 / 0.31 / 1.18 dB), the two arm means (A 11.20, B 9.79
# Mbit/s) and the verdict (-12.6%, FAIL). Those five numbers are exactly what a change to the parser could silently
# break, and each of the three bugs found in this script showed up as one of them moving.
#
# The leg logs live in the gitignored work_tmp, so the test SKIPS (loudly) when they are absent rather than failing:
# a missing corpus is not a broken instrument.
set -u
W=/Users/jiachengwang/dev/ocudu/doc_chinese/llr_ai_detection/work_tmp
PY=$HOME/ai_ce_work/venv/bin/python
HERE=$(cd "$(dirname "$0")" && pwd)
LEGS=$W/legs/r8abba

if [ ! -d "$LEGS" ]; then
  echo "SKIP: no leg logs at $LEGS (the r8 flight's stderr files are not in this workspace)"
  exit 0
fi
out=$($PY "$HERE/flight_verdict.py" --legs "$LEGS" \
        --iperf "r8a1=10.9,r8b1=9.58,r8b2=10.0,r8a2=11.5" --batch r8 2>&1)

fail=0
check() {
  if echo "$out" | grep -qF -e "$1"; then printf '  PASS  %s\n' "$2"; else printf '  FAIL  %s (wanted "%s")\n' "$2" "$1"; fail=$((fail + 1)); fi
}
check "differs  1.04 dB" "adjacent comparability a1-b1 = 1.04 dB"
check "differs  0.31 dB" "adjacent comparability b1-b2 = 0.31 dB"
check "differs  1.18 dB" "adjacent comparability b2-a2 = 1.18 dB"
check "arm A: 2 leg(s), mean 11.20 Mbit/s" "arm A mean = 11.20 Mbit/s"
check "arm B: 2 leg(s), mean 9.79 Mbit/s" "arm B mean = 9.79 Mbit/s"
check "-12.6%" "verdict = -12.6%"
check "FAIL" "verdict = FAIL"
[ "$fail" = 0 ] && echo "SELFTEST PASS" || echo "SELFTEST FAIL ($fail)"
exit "$fail"
