#!/usr/bin/env bash
# ============================================================================
# run_capture.sh — 一键采集（gnb helena + 数据 dump + OAI UE 自动接入与流量）
#
# 流程：起 gnb（helena + OCUDU_HELENA_DUMP_DIR 采集）→ 起 OAI UE（153）→
#      自动检测接入 → 自动 ping + iperf3 上行 →（可中途手动加手机）→
#      Ctrl-C 结束 → 自动 QA 体检 + 存档日志。
#
# 用法:
#   tools/run_capture.sh [--dur SEC] [--tag NAME]
#     --dur    iperf3 上行时长（默认 480 秒）
#     --tag    采集目录后缀（默认自动 site_MMDD_HHMM）
#
# sudo 密码会在 console 提示输入：本机 gnb 一次、153 的 OAI UE 一次。
# 需要你自行准备（脚本会提示）：
#   1) 核心网（open5gs，127 上，10.45.0.1 可达）；
#   2) iperf3 服务端两个端口（127 上）：
#        iperf3 -s -B 10.45.0.1 -p 5201   # 手机用（Termux）
#        iperf3 -s -B 10.45.0.1 -p 5202   # OAI UE 用
#
# 手机（可选，中途手动加入，Termux CLI）：
#   ping -c 5 10.45.0.1
#   iperf3 -c 10.45.0.1 -p 5201 -t <与 OAI UE 相同剩余时长>   # 不要加 -w！
#   （termux-wake-lock 保持前台；跑完 termux-wake-lock -r）
#
# ============ 手动确认数据是否有效（脚本最后也会跑 capture_qa） ============
#   cd ~/ai_ce_work/venv/bin/python
#   python <ai_train>/capture_qa.py <OUT 目录>      # 关注：
#     - rx_meta/dd_meta 行数（越接近越好；CRC-OK 率参考 ~60-99%）
#     - prb 分布（是否覆盖 6-51；53+ 不会出现 = 51 PRB 天花板）
#     - CE input |X| rms（~0.1-0.25 为训练尺度；差 10x 查 rx_gain/UE 功率）
#     - 双 UE 时按 rnti 分账（手机 rnti ≠ OAI UE rnti）
#   确认无效（行数太少/无 CRC-OK/尺度异常）→ 换新目录重跑，不要复用目录。
# ============================================================================
set -u

AI_TRAIN="$(cd "$(dirname "$0")/.." && pwd)"
REPO="$(git -C "$AI_TRAIN" rev-parse --show-toplevel 2>/dev/null)"
REPO="${REPO:-$AI_TRAIN/../../../..}"
VENV_PY="${VENV_PY:-$HOME/ai_ce_work/venv/bin/python}"
UE_HOST="jwang@192.168.100.153"
UE_DIR="~/work/openairinterface5g/cmake_targets/ran_build/build"
UE_CONF="oaiue_b210.conf"          # 3489.42 MHz，与 gnb_uhd_oaiue.yaml 匹配
GNB_CONF="configs/gnb_uhd_oaiue.yaml"
GATEWAY_IP="10.45.0.1"

DUR=480; TAG=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --dur) DUR=$2; shift 2 ;;
    --tag) TAG=$2; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 1 ;;
  esac
done

STAMP=$(date +%m%d_%H%M)
OUT="$HOME/ai_ce_work/capture/site_${STAMP}${TAG:+_$TAG}"
LEGS_DIR="$AI_TRAIN/tools/legs"
mkdir -p "$OUT" "$LEGS_DIR"
GNB_PID=""; UE_PID=""

cleanup() {
  echo; echo "== 收尾 =="
  [[ -n "$GNB_PID" ]] && sudo -n kill "$GNB_PID" 2>/dev/null
  sudo -n pkill -f "build/apps/gnb/gnb -c" 2>/dev/null
  [[ -n "$UE_PID" ]] && kill "$UE_PID" 2>/dev/null
  ssh -o ConnectTimeout=3 "$UE_HOST" "sudo -n pkill nr-uesoftmodem" 2>/dev/null
  echo "gnb/UE 已停止。"
}
trap cleanup EXIT INT

echo "== 前置检查 =="
[[ -x "$REPO/build/apps/gnb/gnb" ]] || { echo "gnb 未构建: $REPO/build/apps/gnb/gnb" >&2; exit 1; }
ssh -o ConnectTimeout=4 -o BatchMode=yes "$UE_HOST" true 2>/dev/null \
  || { echo "无法免密 ssh $UE_HOST（先配好 ssh key）" >&2; exit 1; }
echo "采集目录: $OUT"
echo
echo "请确认已准备："
echo "  1) 核心网 open5gs（127，$GATEWAY_IP 可达）"
echo "  2) iperf3 服务端（127 上）:"
echo "       iperf3 -s -B $GATEWAY_IP -p 5201   # 手机"
echo "       iperf3 -s -B $GATEWAY_IP -p 5202   # OAI UE"
read -r -p "准备好了？[y/N] " ok
[[ "$ok" == "y" || "$ok" == "Y" ]] || { echo "已取消"; exit 1; }

echo
echo "== [1/4] 起 gnb（本机，接下来提示输入 sudo 密码）=="
sudo -v   # 提示密码并缓存凭据
sudo -n env OCUDU_CE_TIME=1 "OCUDU_HELENA_DUMP_DIR=$OUT" \
  "$REPO/build/apps/gnb/gnb" -c "$REPO/$GNB_CONF" \
  expert_phy --pusch_ldpc_decoder_type auto --pusch_channel_estimator_algo helena \
  > "$OUT/gnb_console.log" 2>&1 &
GNB_PID=$!
sleep 6
kill -0 "$GNB_PID" 2>/dev/null || { echo "gnb 启动失败，看 $OUT/gnb_console.log"; exit 1; }
echo "gnb 运行中 (pid $GNB_PID)；日志: /tmp/gnb.log"

echo
echo "== [2/4] 起 OAI UE（153，接下来提示输入 153 的 sudo 密码）=="
ssh -t "$UE_HOST" "sudo -v" 2>/dev/null   # 提示密码并缓存
ssh "$UE_HOST" "cd $UE_DIR && sudo -n ./nr-uesoftmodem -O $UE_CONF" \
  > "$OUT/oaiue_console.log" 2>&1 &
UE_PID=$!
echo "OAI UE 启动中（日志: $OUT/oaiue_console.log，可另开终端 tail -f 观察）"

echo
echo "== [3/4] 等待 OAI UE 接入（检测 /tmp/gnb.log 的 c-rnti 授权，最多 5 分钟）=="
ATTACHED=0
for _ in $(seq 1 60); do
  sleep 5
  if grep -q "UE PUSCH: ue=0 c-rnti" /tmp/gnb.log 2>/dev/null; then
    ATTACHED=1; break
  fi
done
if [[ "$ATTACHED" != "1" ]]; then
  echo "5 分钟内未检测到接入。请检查 RF（天线/位置）后重跑（不要复用本目录）。"
  exit 1
fi
echo "OAI UE 已接入 ✓"

echo
echo "== [4/4] 自动流量: ping 5 次 + iperf3 上行 ${DUR}s（153 → $GATEWAY_IP:5202）=="
ssh "$UE_HOST" "ping -c 5 -W 2 $GATEWAY_IP"
ssh "$UE_HOST" "iperf3 -c $GATEWAY_IP -p 5202 -t $DUR"
echo
echo "OAI UE 流量完成。"
echo "----------------------------------------"
echo "手机（可选）现在可手动接入并打流量（Termux）："
echo "  ping -c 5 $GATEWAY_IP"
echo "  iperf3 -c $GATEWAY_IP -p 5201 -t <剩余时长>   # 不要加 -w"
echo "----------------------------------------"
echo "采集进行中… 完成后按 Ctrl-C 结束本脚本（会自动停 gnb/UE 并做 QA）。"
wait "$UE_PID" 2>/dev/null

echo
echo "== 存档日志 =="
cp /tmp/gnb.log "$LEGS_DIR/leg_$(date +%m%d_%H%M).log" 2>/dev/null && echo "gnb 日志已存: $LEGS_DIR/"

echo
echo "== 自动 QA 体检 =="
"$VENV_PY" "$AI_TRAIN/capture_qa.py" "$OUT" || echo "（capture_qa 失败，请手动检查）"

echo
echo "== 手动确认清单（见脚本头部注释）=="
echo "采集目录: $OUT"
echo "判定有效后，训练用: tools/run_training.sh $OUT"
