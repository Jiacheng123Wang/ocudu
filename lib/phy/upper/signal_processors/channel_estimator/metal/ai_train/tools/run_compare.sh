#!/usr/bin/env bash
# ============================================================================
# run_compare.sh — 一键比较：新采集腿的 PUSCH CRC 与历史腿对照
#
# 用法:
#   tools/run_compare.sh <新腿 gnb 日志路径> [--ref <日志或目录>]
#     --ref  对照日志（默认 tools/legs/ 下所有历史腿）
#
# 输出：每腿 CRC-OK 率 + SINR 分布（可比性校验）+ 多 UE 按 rnti 分账。
# 判定线（历史基线，双 UE / gnb_uhd_oaiue.yaml）：
#   - classical(cpu) 参照：OAI UE 97.5%@19.6dB / 手机 98.6%@13.1dB
#   - SINR 中位差 >3 dB 的对照无效，需重跑对应腿。
# ============================================================================
set -u

AI_TRAIN="$(cd "$(dirname "$0")/.." && pwd)"
VENV_PY="${VENV_PY:-$HOME/ai_ce_work/venv/bin/python}"
NEW=${1:?用法: run_compare.sh <新腿日志> [--ref <日志或目录>]}
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

echo "对照日志:"; printf '  %s\n' "${LOGS[@]}"
if [[ ${#LOGS[@]} -eq 1 ]]; then
  echo "（暂无历史腿——先输出本腿统计；有对照后 tools/legs/ 会一并比较）"
  LOGS+=("$NEW")
fi
echo
"$VENV_PY" "$AI_TRAIN/ab_report.py" "${LOGS[@]}"
