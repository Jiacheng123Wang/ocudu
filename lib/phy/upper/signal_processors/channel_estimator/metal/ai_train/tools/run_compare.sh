#!/usr/bin/env bash
# ============================================================================
# run_compare.sh - one-shot comparison: the new capture leg's PUSCH CRC against the historical legs
#
# usage:
#   tools/run_compare.sh <new leg gnb log path> [--ref <log or directory>]
#     --ref  reference logs (default: every historical leg under tools/legs/)
#
# Output: CRC-OK rate per leg + SINR distribution (comparability check) + per-rnti split with several UEs.
# Verdict lines (historical baseline, two UEs / gnb_uhd_oaiue.yaml):
#   - classical(cpu) reference: OAI UE 97.5%@19.6dB / phone 98.6%@13.1dB
#   - a reference whose median SINR differs by more than 3 dB is invalid: re-run that leg.
# ============================================================================
set -u

AI_TRAIN="$(cd "$(dirname "$0")/.." && pwd)"
VENV_PY="${VENV_PY:-$HOME/ai_ce_work/venv/bin/python}"
NEW=${1:?usage: run_compare.sh <new leg log> [--ref <log or directory>]}
shift
REF="$AI_TRAIN/tools/legs"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --ref) REF=$2; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 1 ;;
  esac
done

LOGS=("$NEW")
if [[ -d "$REF" ]]; then
  for f in "$REF"/*.log; do
    [[ -f "$f" ]] && LOGS+=("$f")
  done
elif [[ -f "$REF" ]]; then
  LOGS+=("$REF")
fi

echo "reference logs:"; printf '  %s\n' "${LOGS[@]}"
if [[ ${#LOGS[@]} -eq 1 ]]; then
  echo "(no historical leg yet - printing this leg's statistics; once there is a reference, tools/legs/ is compared too)"
  LOGS+=("$NEW")
fi
echo
"$VENV_PY" "$AI_TRAIN/ab_report.py" "${LOGS[@]}"
