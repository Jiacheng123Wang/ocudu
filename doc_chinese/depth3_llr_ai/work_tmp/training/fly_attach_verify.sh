#!/bin/bash
# Fly the gNB with one bucketed depth-3 bundle and print a verdict: did the phone attach, did data flow, was the AI arm
# actually computing, how fast, and did the radio stay continuous.
#
# WHY THIS IS ONE COMMAND. A flight answers four independent questions and the evidence sits in four corners of the log;
# `flight_verdict.py` reads all of them, so the result of a leg is a table instead of a grep session. The preflight runs
# first because every failed leg this project has recorded failed on something cheap to check: the radio was not
# plugged in, the core network's AMF was down, or the bundle had no buckets and flew full band.
#
#   fly_attach_verify.sh <bundle> [seconds] [config]
#     bundle   a directory under ~/ai_depth3/gate_ai_models/bundles (must contain <bundle>.mlmodelc and buckets/)
#     seconds  stop after N seconds with ONE SIGINT (default 120; 0 = run until Ctrl-C)
#     config   gnb yaml (default configs/pinned_mcs04.yml)
#
# SUDO IS REQUIRED, and not for the radio: UHD works as a normal user on macOS, but NGAP over SCTP does not -- a gnb
# started without root cannot connect to the AMF (measured 2026-11-28: the no-sudo bench brought the cell up with zero
# radio gaps and still timed out on NG Setup). So the flight goes through sudo, once, in the operator's terminal.
#
# SAFETY ARM BY DEFAULT: OCUDU_DEPTH3_FALLBACK=estimator, so a reception the arm cannot compute goes to the classical
# chain instead of being erased. Export OCUDU_DEPTH3_FALLBACK=none to fly the pure arm for a measurement.
# CHECK_ONLY=1 runs the preflight and stops (no sudo, no transmission).
set -u
NAME=${1:-}
if [ -z "$NAME" ]; then
  sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//' >&2
  exit 2
fi
SECS=${2:-120}
ROOT=$HOME/dev/ocudu
CFG=${3:-$ROOT/configs/pinned_mcs04.yml}
# `default` (or `-`) means THE BUILD'S OWN MODEL: no --pusch_receiver_model, so the arm binds whatever CMake put in
# OCUDU_DEPTH3_DEFAULT_MODEL (today the E4 bundle). It is the arm the air legs used before the 36-block model, and it
# exists precisely so a rig problem can be told apart from a model problem.
USE_DEFAULT=0
case "$NAME" in default|-) USE_DEFAULT=1;; esac
BUNDLE=$HOME/ai_depth3/gate_ai_models/bundles/$NAME/$NAME.mlmodelc
DEFAULT_MODEL=$(sed -n 's/^OCUDU_DEPTH3_DEFAULT_MODEL:FILEPATH=//p' "$PWD/build/CMakeCache.txt" 2>/dev/null)
GNB=$ROOT/build/apps/gnb/gnb
FALLBACK=${OCUDU_DEPTH3_FALLBACK:-estimator}
STAMP=$(date +%Y%m%d_%H%M%S)
OUTDIR=$HOME/ai_depth3/flight_logs/attach_${NAME}_${STAMP}
LOG=$OUTDIR/flight.log

fail() { echo "PREFLIGHT FAIL: $*" >&2; exit 1; }

echo "=== 预检"
/opt/homebrew/bin/uhd_find_devices 2>/dev/null | grep -qiE "B200|B210|USRP" \
  || fail "没有检测到 USRP 射频（uhd_find_devices 为空）—— 插好 B200/B210 再飞"
echo "  射频          OK"
# NGAP IS SCTP, AND `nc` CANNOT SEE AN SCTP LISTENER. The first version of this check used `nc -z` and declared the
# AMF down while open5gs-amfd was listening on 192.168.31.211:38412 -- a false negative that would have refused a good
# flight. The address is read from the config itself, and the check is: ask the AMF host's own `ss -ln --sctp` (ssh
# without a password is available on this rig); if that is not possible, fall back to a TCP probe and then to a WARNING
# rather than a refusal -- the authority on NGAP is the gnb's own `NG Setup`, not a port scan.
AMF_ADDR=$(awk '/^[[:space:]]*addrs:[[:space:]]*/{print $2; exit}' "$CFG")
AMF_HOST=${AMF_ADDR%%:*}
AMF_PORT=${AMF_ADDR##*:}
[ "$AMF_PORT" = "$AMF_ADDR" ] && AMF_PORT=38412
echo "  配置          $CFG  (AMF $AMF_HOST:$AMF_PORT)"
# THE SSH ALIAS, NOT THE BARE IP: the rig reaches the core through `Host nuc5GC` in ~/.ssh/config, and an ssh to the raw
# address needs a host key and a user that ordinary keys do not have. The alias whose HostName is the configured AMF is
# looked up, and `AMF_SSH=<alias>` overrides it.
AMF_SSH=${AMF_SSH:-$(awk -v ip="$AMF_HOST" '/^[Hh]ost[[:space:]]/{host=$2} /^[[:space:]]*[Hh]ost[Nn]ame[[:space:]]/{if ($2==ip) {print host; exit}}' "$HOME/.ssh/config" 2>/dev/null)}
AMF_SSH=${AMF_SSH:-$AMF_HOST}
if ssh -o BatchMode=yes -o ConnectTimeout=4 "$AMF_SSH" "ss -ln --sctp 2>/dev/null | grep -q ':$AMF_PORT'" 2>/dev/null; then
  echo "  AMF $AMF_PORT     OK（SCTP，$AMF_SSH 自报监听）"
elif /usr/bin/nc -z -G 2 "$AMF_HOST" "$AMF_PORT" >/dev/null 2>&1; then
  echo "  AMF $AMF_PORT     OK（TCP 探测）"
else
  echo "  AMF $AMF_PORT     ★ 无法确认（NGAP 是 SCTP，nc 看不到；以 gnb 的 NG Setup 为准）" >&2
fi
[ -x "$GNB" ] || fail "没有 $GNB"
[ -f "$CFG" ] || fail "没有配置: $CFG"
if lsof -nP -i :2152 >/dev/null 2>&1; then
  fail "GTP-U 2152 已被占用（另一个 gnb 还在跑？）：$(lsof -nP -i :2152 | tail -1)"
fi
if [ "$USE_DEFAULT" = "1" ]; then
  [ -n "$DEFAULT_MODEL" ] || fail "这个构建没有默认模型（CMakeCache 里没有 OCUDU_DEPTH3_DEFAULT_MODEL）"
  [ -d "$DEFAULT_MODEL" ] || fail "默认模型不存在: $DEFAULT_MODEL"
  BUCKETS=$(ls -d "$(dirname "$DEFAULT_MODEL")"/buckets/base_w*.mlmodelc 2>/dev/null | wc -l | tr -d ' ')
  echo "  bundle        构建默认：$(basename "$(dirname "$DEFAULT_MODEL")")/$(basename "$DEFAULT_MODEL")  buckets=$BUCKETS"
else
  [ -d "$BUNDLE" ] || fail "没有 bundle: $BUNDLE"
  [ -d "$HOME/ai_depth3/gate_ai_models/bundles/$NAME/buckets" ] \
    || fail "bundle 里没有 buckets/：会按全带跑"
  BUCKETS=$(ls -d "$HOME/ai_depth3/gate_ai_models/bundles/$NAME"/buckets/base_w*.mlmodelc 2>/dev/null | wc -l | tr -d ' ')
  echo "  bundle        $(basename "$BUNDLE")  buckets=$BUCKETS"
fi
LOAD=$(sysctl -n vm.loadavg | awk '{print $2}')
# REAL-TIME FAILURES ARE A HOST PROPERTY, NOT A MODEL ONE. Measured 2026-11-28: 7 703 `Real-time failure in RF` lines in
# one leg (late + underflow) while an idle leg on the same rig had zero, with Chrome/WindowServer consuming ~60 % CPU.
# A leg with those is not a measurement of anything, so the warning is loud and names the likely culprits.
echo "  load average  ${LOAD}  $([ "${LOAD%%.*}" -gt 2 ] && echo '★ 偏高：射频实时性可能不保（见日志 Real-time failure in RF）')"
echo "  本机前三大 CPU：$(ps ax -o pcpu,comm -r | sed -n '2,4p' | awk '{printf "%s(%s%%) ", $2, $1}')"
ARM=${ARM:-ai}
echo "  臂            $ARM  $([ "$ARM" = "classic" ] && echo '（--phy_pipeline cpu：经典链对照，不绑模型）' || echo '（--phy_pipeline npu ＋ 模型）')"

if [ "${CHECK_ONLY:-0}" = "1" ]; then
  echo "  CHECK_ONLY=1：预检通过，未起飞"
  exit 0
fi

mkdir -p "$OUTDIR"
{
  echo "# leg:    $OUTDIR"
  if [ "$USE_DEFAULT" = "1" ]; then
    echo "# bundle: <build default> $DEFAULT_MODEL"
  else
    echo "# bundle: $NAME"
  fi
  echo "# bundle files sha256: $(cd "$HOME/ai_depth3/gate_ai_models/bundles/$NAME" && find . -type f -print0 | sort -z | xargs -0 shasum -a 256 | shasum -a 256 | cut -c1-16)"
  echo "# config: $CFG"
  echo "# gnb:    $GNB sha256 $(shasum -a 256 "$GNB" | cut -c1-16)"
  echo "# fallback: $FALLBACK   pipeline: npu   timing: 1"
  echo "# config diff (工作树):"; git -C "$ROOT" diff -- configs/ | sed 's/^/#   /'
  echo "# load at start: $(sysctl -n vm.loadavg)"
  echo "# top cpu at start:"; ps ax -o pcpu,comm -r | sed -n '2,5p' | sed 's/^/#   /'
  echo "# started $(date)"
} > "$LOG"

echo "=== 起飞（$SECS s；日志 ${LOG}）"
# THE APP'S OWN INFO LOG IS POINTED AT THE LEG DIRECTORY (`--log.filename`). It used to be the config's /tmp/gnb.log,
# which is root-owned after a sudo run: the next leg could not remove it (`rm: Permission denied`), so a stale
# `receiver=ai` could be read as this flight's, and the rich NGAP/RRC/MAC/RF lines were not archived with the leg.
ARGS=(-c "$CFG" --log.filename "$OUTDIR/gnb_full.log")
if [ "$ARM" = "classic" ]; then
  ARGS+=(--expert_phy.phy_pipeline cpu)
else
  ARGS+=(--expert_phy.phy_pipeline npu)
  [ "$USE_DEFAULT" = "0" ] && ARGS+=(--expert_phy.pusch_receiver_model "$BUNDLE")
fi
# THE PATH IS `--cu_cp.…`, NOT `--…`: the option is registered on the CU-CP subcommand (measured 2026-11-28:
# `--request_pdu_session_timeout 10` made CLI11 print "The following arguments were not expected" and the app exited
# before touching the radio -- which reads exactly like "gnb 没连上 AMF".
[ -n "${PDU_TIMEOUT:-}" ] && ARGS+=(--cu_cp.request_pdu_session_timeout "$PDU_TIMEOUT")
# THE INFO LOG IS NOT FREE. The 2026-11-28 17:20 leg wrote 13.9 MB in three minutes (154 872 lines, ~860 lines/s) while the
# radio was already missing deadlines; `LOG_LEVEL=warning` keeps the leg light and the verdict's own evidence (the receiver
# prints to stderr, and `--log.all_level` only governs the file logger) intact.
[ -n "${LOG_LEVEL:-}" ] && ARGS+=(--log.all_level "$LOG_LEVEL")

# ASK FOR THE PASSWORD IN THE FOREGROUND FIRST. A background job that tries to read from the controlling terminal gets
# SIGTTIN and stops, so a `sudo` in the background cannot prompt -- it either fails outright or (if the timestamp is
# still cached) proceeds, which makes the whole flight depend on how recently the operator last used sudo. `sudo -v`
# here prompts where prompting works and leaves the credential cached for the background launch and the later SIGINT.
echo "  （下面先要一次 sudo 密码，然后后台起 gnb；从 pid 出现起计时 ${SECS}s）"
sudo -v || { echo "★ sudo 认证失败" >&2; exit 1; }
sudo env "OCUDU_DEPTH3_TIMING=1" "OCUDU_DEPTH3_FALLBACK=$FALLBACK" "$GNB" "${ARGS[@]}" >> "$LOG" 2>&1 &
GNB_WRAPPER=$!

# WAITING FOR THE PID MUST OUTLAST THE SUDO PROMPT AND THE FPGA LOAD. The first version waited 8 s, found no pid and
# killed the wrapper -- which read as "gnb 没起来" while the log actually held a complete (zero-work) shutdown report,
# i.e. the gnb HAD started and was killed before it did anything. `sudo` asks for the password on the terminal and only
# then execs, and the B200 FPGA image takes ~10-15 s, so the wait is a poll, not a fixed sleep.
GNB_PID=""
for _ in $(seq 1 60); do
  # A COMMAND LINE THE APP REFUSED IS NOT A SLOW START. CLI11 exits immediately with a one-line reason, and waiting the
  # rest of the poll window hides it behind "gnb 没起来" (which is what made the 17:16 leg look like an AMF problem).
  if grep -qa "arguments were not expected" "$LOG"; then
    echo "★ 命令行被拒（不是 AMF 的问题）：" >&2
    grep -a -A1 "arguments were not expected" "$LOG" | head -3 >&2
    kill "$GNB_WRAPPER" 2>/dev/null; exit 2
  fi
  GNB_PID=$(pgrep -P "$GNB_WRAPPER" 2>/dev/null | head -1)
  [ -z "$GNB_PID" ] && GNB_PID=$(pgrep -f "$(basename "$GNB") -c $CFG" 2>/dev/null | head -1)
  [ -n "$GNB_PID" ] && break
  kill -0 "$GNB_WRAPPER" 2>/dev/null || break
  sleep 1
done
if [ -z "$GNB_PID" ]; then
  echo "★ gnb 没起来（sudo 没输密码？看日志尾）：" >&2; tail -6 "$LOG" >&2
  kill "$GNB_WRAPPER" 2>/dev/null; exit 1
fi
echo "  gnb pid ${GNB_PID}；跑 ${SECS}s"
sleep "$SECS"
if kill -0 "$GNB_PID" 2>/dev/null; then
  echo "  到点：发一次 SIGINT（等 Stopping... 与 summary）"
  sudo kill -INT "$GNB_PID" 2>/dev/null || kill -INT "$GNB_PID" 2>/dev/null
else
  echo "  gnb 已自行退出，直接判定"
fi
wait "$GNB_WRAPPER" 2>/dev/null
stty sane 2>/dev/null || true

# THE CONFIG'S OWN LOG FILE carries the INFO-level startup lines (including `receiver=ai`), which the console does not:
# `log.all_level: info` writes to `log.filename`, and the console sink is a different level. Only the three lines the
# verdict needs are appended, so no counted pattern is duplicated.
# THE CONFIG'S LOG IS THE RICH ONE (INFO: NGAP/RRC/MAC/PHY/RF), and it is what the verdict needs to say who released the
# UE and whether the radio kept its deadlines. It is archived with the leg rather than grepped into the console log, and
# both are handed to the analyzer (it parses each file and merges; no line is counted twice).
GNB_FULL="$OUTDIR/gnb_full.log"
[ -f "$GNB_FULL" ] || GNB_FULL=""

echo
echo "=== 判定"
VERDICT_LOGS=("$LOG"); [ -n "$GNB_FULL" ] && VERDICT_LOGS+=("$GNB_FULL")
python3 "$ROOT/doc_chinese/depth3_llr_ai/work_tmp/analysis/flight_verdict.py" "${VERDICT_LOGS[@]}"
VERDICT=$?
echo
echo "=== 日志：$LOG"
[ "$VERDICT" -eq 0 ] || echo "★ 判定不是 GO：看上面 [1]–[4]，不要只看 [5]" >&2
exit "$VERDICT"
