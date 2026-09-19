#!/usr/bin/env bash
# ============================================================================
# run_capture.sh - one-shot capture run (gnb helena + data dump + OAI UE attach and traffic)
#
# Flow: start gnb (helena + OCUDU_HELENA_DUMP_DIR capture) -> start the OAI UE (153) ->
#       wait for the attach automatically -> automatic ping + iperf3 uplink -> (the phone can be
#       added by hand mid-run) -> Ctrl-C ends it -> automatic QA check + log archiving.
#
# usage:
#   tools/run_capture.sh [--dur SEC] [--tag NAME]
#     --dur    iperf3 uplink duration (default 480 s)
#     --tag    suffix of the capture directory (default: auto site_MMDD_HHMM)
#
# The sudo password is asked for on the console: once for the local gnb, once for the OAI UE on 153.
# Things you have to prepare yourself (the script reminds you):
#   1) the core network (open5gs, on 127, 10.45.0.1 reachable);
#   2) two iperf3 servers (on 127):
#        iperf3 -s -B 10.45.0.1 -p 5201   # for the phone (Termux)
#        iperf3 -s -B 10.45.0.1 -p 5202   # for the OAI UE
#
# The phone (optional, joined by hand mid-run, Termux CLI):
#   ping -c 5 10.45.0.1
#   iperf3 -c 10.45.0.1 -p 5201 -t <same remaining time as the OAI UE>   # do NOT pass -w!
#   (keep it in the foreground with termux-wake-lock; release with termux-wake-lock -r afterwards)
#
# ============ Checking by hand whether the data is usable (the script runs capture_qa too) ======
#   cd ~/ai_ce_work/venv/bin/python
#   python <ai_train>/capture_qa.py <OUT directory>      # look at:
#     - the rx_meta/dd_meta line counts (the closer to full the better; CRC-OK rate ~60-99%)
#     - the prb distribution (does it cover 6-51; 53+ never appears = the 51 PRB ceiling)
#     - the CE input |X| rms (~0.1-0.25 is the training scale; 10x off -> check rx_gain/UE power)
#     - with two UEs, split by rnti (the phone's rnti != the OAI UE's rnti)
#   If it is not usable (too few lines / no CRC-OK / odd scale) -> start over in a NEW directory,
#   never reuse one.
# ============================================================================
set -u

AI_TRAIN="$(cd "$(dirname "$0")/.." && pwd)"
REPO="$(git -C "$AI_TRAIN" rev-parse --show-toplevel 2>/dev/null)"
REPO="${REPO:-$AI_TRAIN/../../../..}"
VENV_PY="${VENV_PY:-$HOME/ai_ce_work/venv/bin/python}"
UE_HOST="jwang@192.168.100.153"
UE_DIR="~/work/openairinterface5g/cmake_targets/ran_build/build"
UE_CONF="oaiue_b210.conf"          # 3489.42 MHz, matching gnb_uhd_oaiue.yaml
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
  echo; echo "== wrapping up =="
  [[ -n "$GNB_PID" ]] && sudo -n kill "$GNB_PID" 2>/dev/null
  sudo -n pkill -f "build/apps/gnb/gnb -c" 2>/dev/null
  [[ -n "$UE_PID" ]] && kill "$UE_PID" 2>/dev/null
  ssh -o ConnectTimeout=3 "$UE_HOST" "sudo -n pkill nr-uesoftmodem" 2>/dev/null
  echo "gnb/UE stopped."
}
trap cleanup EXIT INT

echo "== preflight =="
[[ -x "$REPO/build/apps/gnb/gnb" ]] || { echo "gnb not built: $REPO/build/apps/gnb/gnb" >&2; exit 1; }
ssh -o ConnectTimeout=4 -o BatchMode=yes "$UE_HOST" true 2>/dev/null \
  || { echo "cannot ssh $UE_HOST without a password (set up the ssh key first)" >&2; exit 1; }
echo "capture directory: $OUT"
echo
echo "please confirm these are ready:"
echo "  1) core network open5gs (127, $GATEWAY_IP reachable)"
echo "  2) iperf3 servers (on 127):"
echo "       iperf3 -s -B $GATEWAY_IP -p 5201   # phone"
echo "       iperf3 -s -B $GATEWAY_IP -p 5202   # OAI UE"
read -r -p "ready? [y/N] " ok
[[ "$ok" == "y" || "$ok" == "Y" ]] || { echo "cancelled"; exit 1; }

echo
echo "== [1/4] starting gnb (local; the sudo password is asked for next) =="
sudo -v   # ask for the password and cache the credentials
sudo -n env OCUDU_CE_TIME=1 "OCUDU_HELENA_DUMP_DIR=$OUT" \
  "$REPO/build/apps/gnb/gnb" -c "$REPO/$GNB_CONF" \
  expert_phy --pusch_ldpc_decoder_type auto --pusch_channel_estimator_algo helena \
  > "$OUT/gnb_console.log" 2>&1 &
GNB_PID=$!
sleep 6
kill -0 "$GNB_PID" 2>/dev/null || { echo "gnb failed to start, see $OUT/gnb_console.log"; exit 1; }
echo "gnb running (pid $GNB_PID); log: /tmp/gnb.log"

echo
echo "== [2/4] starting the OAI UE (153; its sudo password is asked for next) =="
ssh -t "$UE_HOST" "sudo -v" 2>/dev/null   # ask for the password and cache it
ssh "$UE_HOST" "cd $UE_DIR && sudo -n ./nr-uesoftmodem -O $UE_CONF" \
  > "$OUT/oaiue_console.log" 2>&1 &
UE_PID=$!
echo "OAI UE starting (log: $OUT/oaiue_console.log; tail -f it from another terminal)"

echo
echo "== [3/4] waiting for the OAI UE to attach (watching /tmp/gnb.log for a c-rnti grant, max 5 min) =="
ATTACHED=0
for _ in $(seq 1 60); do
  sleep 5
  if grep -q "UE PUSCH: ue=0 c-rnti" /tmp/gnb.log 2>/dev/null; then
    ATTACHED=1; break
  fi
done
if [[ "$ATTACHED" != "1" ]]; then
  echo "no attach within 5 minutes. Check the RF (antennas/placement) and run again (in a new directory)."
  exit 1
fi
echo "OAI UE attached"

echo
echo "== [4/4] automatic traffic: ping 5x + iperf3 uplink ${DUR}s (153 -> $GATEWAY_IP:5202) =="
ssh "$UE_HOST" "ping -c 5 -W 2 $GATEWAY_IP"
ssh "$UE_HOST" "iperf3 -c $GATEWAY_IP -p 5202 -t $DUR"
echo
echo "OAI UE traffic done."
echo "----------------------------------------"
echo "the phone (optional) can now attach and generate traffic by hand (Termux):"
echo "  ping -c 5 $GATEWAY_IP"
echo "  iperf3 -c $GATEWAY_IP -p 5201 -t <remaining time>   # do NOT pass -w"
echo "----------------------------------------"
echo "capture in progress... press Ctrl-C when done (it stops gnb/UE and runs the QA)."
wait "$UE_PID" 2>/dev/null

echo
echo "== archiving logs =="
cp /tmp/gnb.log "$LEGS_DIR/leg_$(date +%m%d_%H%M).log" 2>/dev/null && echo "gnb log stored: $LEGS_DIR/"

echo
echo "== automatic QA =="
"$VENV_PY" "$AI_TRAIN/capture_qa.py" "$OUT" || echo "(capture_qa failed, check by hand)"

echo
echo "== manual check list (see the header comment of this script) =="
echo "capture directory: $OUT"
echo "once it is judged usable, train with: tools/run_training.sh $OUT"
