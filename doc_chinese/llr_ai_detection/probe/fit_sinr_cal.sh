#!/usr/bin/env bash
# Fit the per-modulation SINR calibration of a run against TRUTH, then cross-check it on air.
#
# WHY BOTH. Two fits disagree in this project and only one of them is a calibration:
#
#   against truth     the synthetic units carry the SNR they were built at (`snr_nominal`) and the noise
#                     they were built with (`sigma2_over_r2`), so an affine fitted here corrects the head's
#                     own bias. It is the fit that means something, but it needs the training data to
#                     COVER the range -- the 2026-10-19 run whose 64QAM units stopped at 24 dB produced a
#                     head that saturated at 24.4, and no affine can put back a range that is not there
#                     (design section 19.220(e)).
#   against the chain  the classical chain's publication on the same receptions. It is not truth -- the
#                     chain carries its own bias -- but it is the number AMC was tuned against, so the fit
#                     is the OPERATIONAL target. Reported as a cross-check, never as the calibration.
#
# A slope far from 1 in the truth fit means the head is compressed (fix the data, not the knob); a slope
# near 1 with a constant offset is a bias, which is exactly what the knob exists for.
#
# Usage:
#   fit_sinr_cal.sh <run dir>            e.g. fit_sinr_cal.sh work_tmp/data/runs/d3_dmrs2711h_c32
#     CAL=<dir with qm<N>.npz>           per-modulation calibration subsets (built by build_cal_subsets)
#     CSVS="a.csv b.csv"                 air CSVs for the operational cross-check
set -u -o pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
W=$ROOT/doc_chinese/llr_ai_detection/work_tmp
PROBE=$ROOT/doc_chinese/llr_ai_detection/probe
PY=$HOME/ai_ce_work/venv/bin/python
RUN=${1:?usage: fit_sinr_cal.sh <run dir>}
CAL=${CAL:-/tmp/cal2}

model=$RUN/depth3_best.keras
[ -f "$model" ] || { echo "no $model" >&2; exit 2; }

echo "=== truth fit: published SINR against the prescribed SNR (per modulation)"
entries=()
for spec in "2 QPSK" "4 16QAM" "6 64QAM" "8 256QAM"; do
  set -- $spec
  qm=$1; mod=$2
  [ -f "$CAL/qm$qm.npz" ] || { echo "  qm=$qm: no calibration subset at $CAL/qm$qm.npz"; continue; }
  echo "--- qm=$qm ($mod)"
  out=$("$PY" "$PROBE/heads_vs_prescribed.py" --model "$model" --data "$CAL/qm$qm.npz" \
          --modulation "$mod" --emit-sinr-cal 2>/dev/null)
  printf '%s\n' "$out" | grep -E "worst held-out|^--sinr-cal" | sed 's/^/    /'
  line=$(printf '%s\n' "$out" | grep -oE '^--sinr-cal "[^"]+"' | sed 's/^--sinr-cal "//; s/"$//')
  [ -n "$line" ] && entries+=("$line")
done

if [ ${#entries[@]} -eq 0 ]; then
  echo "no modulation produced a calibration entry" >&2
  exit 1
fi
joined=$(IFS=';'; echo "${entries[*]}")
echo
echo "=== the declaration to pass to the export"
echo "  --sinr-cal \"$joined\""

if [ -n "${CSVS:-}" ]; then
  echo
  echo "=== cross-check: the arm's publication against the classical chain on air"
  # shellcheck disable=SC2086
  "$PY" "$PROBE/reporting_range.py" --label "arm on air" $(for c in $CSVS; do printf -- "--ab %s " "$c"; done)
  echo
  echo "  A slope far from 1 in the truth fit above means the spread is missing: fix the data, not the knob."
fi
