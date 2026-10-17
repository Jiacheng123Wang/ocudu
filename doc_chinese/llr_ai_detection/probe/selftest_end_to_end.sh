#!/usr/bin/env bash
# The whole reading path, once, on two synthetic batches whose verdicts are known by hand.
#
# selftest_flight_verdict.sh checks the PARSER on small fixtures. This checks the READING: two batches shaped like the
# ones the operator will fly, each arm pointed at a different branch of the disposition table, with the expected verdict
# lines asserted word for word. If the tooling is going to be trusted with a phone session, it should first reproduce a
# case that was computed by hand.
#
# The two batches:
#   batch one  a/c, the receiver contrast   A ~11.1, C ~6.4         -> "AI arm is BETTER by 42.3%"
#   batch two  a/b/c/d, four arms           B +3%  (inside the band) -> NO MEASURABLE DIFFERENCE
#                                           C -18% (outside)         -> "AI arm is BETTER by 18.0%"
#                                           D -0.5% (inside)         -> NO MEASURABLE DIFFERENCE
set -u
PY=${PY:-$HOME/ai_ce_work/venv/bin/python}
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(cd "$HERE/../work_tmp" && pwd)
SOAK_A=$W/tmp/soak/pubtest_ft/stderr.txt      # the deployed bundle's instrument output
SOAK_B=$W/tmp/soak/pubtest_cal/stderr.txt     # the calibrated bundle's
BIGLOG=$W/legs/r8abba/gnb_gpu_r8a1_1017_0951.log   # only for the PUSCH-line fallback
if [ ! -f "$SOAK_A" ] || [ ! -f "$SOAK_B" ]; then
  echo "SKIP: the soak logs this test builds its legs from are not in this workspace"
  exit 0
fi
work=$W/tmp/e2e
rm -rf "$work"; mkdir -p "$work"
fail=0
check() {
  if grep -qF -e "$1" "$2"; then printf '  PASS  %s\n' "$3"; else printf '  FAIL  %s (wanted "%s")\n' "$3" "$1"; fail=$((fail + 1)); fi
}

make_leg() {   # make_leg <dir> <tag> <a|b|c|d>
  local dir=$1 tag=$2 arm=$3
  case "$arm" in
    a|b) cp "$SOAK_A" "$dir/gnb_gpu_${tag}.log.stderr" ;;
    c)   : > "$dir/gnb_gpu_${tag}.log.stderr" ;;
    d)   cp "$SOAK_B" "$dir/gnb_gpu_${tag}.log.stderr" ;;
  esac
  if [ "$arm" = c ]; then : > "$dir/gnb_gpu_${tag}.log"; else ln -sf "$BIGLOG" "$dir/gnb_gpu_${tag}.log"; fi
  # `c` legs have no depth3 lines at all, which is what makes the RTT fallback the comparability basis there
  if [ "$arm" = b ] || [ "$arm" = d ]; then cp "$SOAK_B" "$dir/gnb_gpu_${tag}.log.stderr"; fi
  # A SOAK LOG IS NOT A FLOWN LEG: it has no `gNB options` line, so arm d's net marker has no source and the arm check
  # refuses the leg. Append the line a real flight prints, in the canonical `=` spelling that a real log actually uses.
  if [ "$arm" = d ]; then
    echo "gNB options   : --expert_phy.pusch_receiver_backend=ai --expert_phy.pusch_reporting_source=net --expert_phy.pusch_receiver_model=/models/d3_modcond_cal.mlmodelc" >> "$dir/gnb_gpu_${tag}.log.stderr"
  fi
}

echo "== batch one: the receiver contrast (a c c a)"
d1=$work/r9r; mkdir -p "$d1"
make_leg "$d1" r9ra1_1017_1200 a; make_leg "$d1" r9rc1_1017_1205 c
make_leg "$d1" r9rc2_1017_1210 c; make_leg "$d1" r9ra2_1017_1215 a
$PY "$HERE/flight_verdict.py" --legs "$d1" --iperf "r9ra1=11.0,r9rc1=6.3,r9rc2=6.5,r9ra2=11.2" \
  --ping "r9ra1=34.6,r9rc1=34.8,r9rc2=34.5,r9ra2=34.6" --batch r9r > "$work/one.txt" 2>&1
check "arm A: 2 leg(s), mean 11.10 Mbit/s" "$work/one.txt" "arm A mean"
check "arm C: 2 leg(s), mean 6.40 Mbit/s"  "$work/one.txt" "arm C mean"
check "AI arm is BETTER by 42.3%"          "$work/one.txt" "the reading names the AI arm as better"
check "comparability metric: rtt"          "$work/one.txt" "the classical legs force the RTT basis"

echo "== batch two: four arms (a b c d d c b a)"
d2=$work/r9c; mkdir -p "$d2"
make_leg "$d2" r9ca1_1017_1200 a; make_leg "$d2" r9cb1_1017_1205 b
make_leg "$d2" r9cc1_1017_1210 c; make_leg "$d2" r9cd1_1017_1215 d
make_leg "$d2" r9cd2_1017_1220 d; make_leg "$d2" r9cc2_1017_1225 c
make_leg "$d2" r9cb2_1017_1230 b; make_leg "$d2" r9ca2_1017_1235 a
$PY "$HERE/flight_verdict.py" --legs "$d2" --batch r9c \
  --iperf "r9ca1=11.0,r9cb1=11.4,r9cc1=9.0,r9cd1=11.05,r9cd2=11.0,r9cc2=9.2,r9cb2=11.3,r9ca2=11.2" \
  --ping "r9ca1=34.6,r9cb1=34.7,r9cc1=34.5,r9cd1=34.8,r9cd2=34.6,r9cc2=34.7,r9cb2=34.6,r9ca2=34.5" \
  > "$work/two.txt" 2>&1
check "arm A: 2 leg(s), mean 11.10 Mbit/s" "$work/two.txt" "four-arm batch: arm A mean"
check "arm B: 2 leg(s), mean 11.35 Mbit/s" "$work/two.txt" "four-arm batch: arm B mean"
check "arm D: 2 leg(s), mean 11.03 Mbit/s" "$work/two.txt" "four-arm batch: arm D mean"
check "B vs A: 11.35 vs 11.10 Mbit/s = +2.3%" "$work/two.txt" "B's arithmetic"
check "NO MEASURABLE DIFFERENCE" "$work/two.txt" "a small difference is not called a result"
check "AI arm is BETTER by 18.0%" "$work/two.txt" "C is read, not PASS/FAILed"
check "[R6's delivery form (calibrated + net)]" "$work/two.txt" "D carries its pre-registered meaning"

echo
if [ "$fail" = 0 ]; then
  echo "END TO END PASS"
else
  echo "END TO END FAIL ($fail) -- the two runs' output is in $work/{one,two}.txt"
fi
exit "$fail"
