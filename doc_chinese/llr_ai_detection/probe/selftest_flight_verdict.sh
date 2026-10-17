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
        --iperf "r8a1=10.9,r8b1=9.58,r8b2=10.0,r8a2=11.5" --batch r8 --arm-scheme legacy 2>&1)

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
# A SECOND fixture, synthetic and committed in the script itself: the real leg logs on disk predate the
# `[depth3-sinr]` instrument, so without this the publication column -- the one number the flight exists to compare --
# would be untested. The lines below are the arm's exact formats (taken from a 2100-reception soak), and the fixture
# is rebuilt in a temp dir on every run so it cannot rot.
# THE DETECTOR READS THREE LITERAL SUBSTRINGS the gNB prints at startup: the arm banner ("AI ARM ACTIVE"), the
# bundle's calibration note ("CALIBRATED by the bundle"), and the echoed command line ("--pusch_reporting_source net").
# A real log supplies them; a synthetic leg has to print the same strings or it is testing a log nobody produces. This
# helper emits them per arm, in the real wording, and -- importantly -- emits NOTHING for arm c, whose log has none,
# so the "classical leg" fixtures stay honest about the absence.
arm_banner() {
  local backend=classic source=classic bundle=none
  if [ "$1" != c ]; then
    backend=ai
    bundle=d3_modcond_ft.mlmodelc
    echo "[pusch_receiver] backend=ai: AI ARM ACTIVE over 4 model(s). Each bound model COMPUTES channel estimation, equalisation and demapping for the modulation it declares; the classical demodulator does not run for those receptions."
  fi
  if [ "$1" = b ] || [ "$1" = d ]; then
    bundle=d3_modcond_cal.mlmodelc
    echo "[pusch_receiver] backend=ai: the published SINR is CALIBRATED by the bundle (published' = slope * published + offset, dB): Qm=2 slope 1.004 offset +0.010, Qm=4 slope 1.010 offset +0.020, Qm=6 slope 1.062 offset +0.040, Qm=8 slope 1.137 offset +0.070; the soft bits are not affected"
  fi
  # THE CANONICAL SPELLING, with `=`, because that is what a real `gNB options :` line prints and the one the first
  # version of the marker test could not see.
  [ "$1" = d ] && source=net
  echo "gNB options   : --expert_phy.pusch_receiver_backend=$backend --expert_phy.pusch_reporting_source=$source --expert_phy.pusch_receiver_model=/models/$bundle"
}

FIXTURE=$(mktemp -d -t verdict_fixture_XXXXXX)
mkdir -p "$FIXTURE/legs"
{ arm_banner b; cat <<'FIXTURE_EOF'
[depth3-measure] reception 1 (Qm=8 256QAM) over 612 pilot RE(s): epre net=-2.58 classic=-2.58 dB | rsrp net=-2.55 classic=-2.61 dB | snr net=21.92 classic=26.00 dB | noise net=8.9879e-04 classic=6.9088e-04 residual=4.0276e-03 plane_units=2.0198e-03 (570 residual(s)) | snr plane-only=23.95 dB
[depth3-sinr] 2000 reception(s): PUBLISHED SINR raw p10 23.08 p50 23.08 p90 23.08 dB | calibrated p10 26.31 p50 26.31 p90 26.31 dB | calibration APPLIED (this is the number the scheduler acts on)
[depth3-timing] 2000 forward(s), microseconds: plane p50 39 p99 50 max 60 | forward p50 1079 p99 1200 max 1300 | deliver p50 112 p99 150 max 200 | predict p50 1050 p99 1150 max 1250 |
FIXTURE_EOF
} > "$FIXTURE/legs/gnb_gpu_r9b1_1017_1200.log.stderr"
fixture_out=$($PY "$HERE/flight_verdict.py" --legs "$FIXTURE/legs" --iperf "r9b1=9.6" --batch r9 2>&1)
rm -rf "$FIXTURE"
check_in() {
  if echo "$2" | grep -qF -e "$1"; then printf '  PASS  %s\n' "$3"; else printf '  FAIL  %s (wanted "%s")\n' "$3" "$1"; fail=$((fail + 1)); fi
}
check_in "23.08 -> 26.31 (APPLIED)" "$fixture_out" "publication column: raw 23.08 -> calibrated 26.31 (APPLIED)"
check_in "26.00" "$fixture_out" "comparability from the measure line"
check_in "1079" "$fixture_out" "forward p50 parsed from the timing line"

# A THIRD fixture: the receiver-contrast batch -- an AI leg and a CLASSICAL leg, which has no `[depth3-measure]` line
# at all and, in a build without the PHY's per-reception `PUSCH: ... sinr=` line, no SNR basis either. The comparability
# then has to fall back to ping RTT, and the classical arm has to be READ rather than PASS/FAILed.
MIXED=$(mktemp -d -t verdict_mixed_XXXXXX)
mkdir -p "$MIXED/legs"
{ arm_banner a; printf '%s\n' '[depth3-measure] reception 1 (Qm=8 256QAM) over 612 pilot RE(s): epre net=-2.58 classic=-2.58 dB | rsrp net=-2.55 classic=-2.61 dB | snr net=21.92 classic=26.00 dB | noise net=8.9879e-04 classic=6.9088e-04 residual=4.0276e-03 plane_units=2.0198e-03 (570 residual(s)) | snr plane-only=23.95 dB'; } > "$MIXED/legs/gnb_gpu_r9a1_1017_1200.log.stderr"
: > "$MIXED/legs/gnb_gpu_r9a1_1017_1200.log"
: > "$MIXED/legs/gnb_gpu_r9c1_1017_1205.log.stderr"
: > "$MIXED/legs/gnb_gpu_r9c1_1017_1205.log"
mixed_out=$($PY "$HERE/flight_verdict.py" --legs "$MIXED/legs" --iperf "r9a1=10.9,r9c1=6.2" \
              --ping "r9a1=34.6,r9c1=35.1" --batch r9 2>&1)
rm -rf "$MIXED"
check_in "comparability metric: rtt" "$mixed_out" "mixed batch falls back to the ping RTT basis"
check_in "AI arm is BETTER by 43.1%" "$mixed_out" "the classical arm is READ as a reference, not PASS/FAILed"

# A FOURTH fixture: one leg per arm, a/b/c/d. The leg-name pattern originally ended at `c`, so the eight-leg batch's
# `d` legs were dropped silently and the verdict answered a three-arm question. This asserts arm D is judged.
FOUR=$(mktemp -d -t verdict_four_XXXXXX)
mkdir -p "$FOUR/legs"
for spec in a1:1200 b1:1205 c1:1210 d1:1215; do
  arm=${spec%%:*}; stamp=${spec##*:}
  { arm_banner "${arm%%[0-9]*}"; printf '%s\n' '[depth3-measure] reception 1 (Qm=8 256QAM) over 612 pilot RE(s): epre net=-2.58 classic=-2.58 dB | rsrp net=-2.55 classic=-2.61 dB | snr net=21.92 classic=26.00 dB | noise net=8.9879e-04 classic=6.9088e-04 residual=4.0276e-03 plane_units=2.0198e-03 (570 residual(s)) | snr plane-only=23.95 dB'; } > "$FOUR/legs/gnb_gpu_r9x${arm}_1017_${stamp}.log.stderr"
  : > "$FOUR/legs/gnb_gpu_r9x${arm}_1017_${stamp}.log"
done
four_out=$($PY "$HERE/flight_verdict.py" --legs "$FOUR/legs" --iperf "r9xa1=10.0,r9xb1=10.1,r9xc1=10.2,r9xd1=10.3" --batch r9x 2>&1)
rm -rf "$FOUR"
check_in "arm D: 1 leg(s)" "$four_out" "an arm-d leg is parsed and grouped (it was silently dropped before)"
check_in "[R6's delivery form" "$four_out" "arm D's pre-registered meaning is printed"
# The four-leg fixture has a measure line but no timing line, i.e. no reception: the health check must say so rather
# than reading a throughput off a leg that received nothing.
check_in "NO reception in its log" "$four_out" "a leg with no reception line is flagged as vacuous"

[ "$fail" = 0 ] && echo "SELFTEST PASS" || echo "SELFTEST FAIL ($fail)"
exit "$fail"
