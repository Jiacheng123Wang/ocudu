#!/usr/bin/env bash
# Capture ONE successful attach, in CLASSIC mode, and stop the gNB by itself when the attach completes.
#
# WHY THIS EXISTS. Every live leg so far recorded the traffic AFTER attach, so the only part of the
# uplink that was never replayed offline is the part that fails: Msg3, the RRC/capability/registration
# chain, narrow allocations, low MCS, and the retransmissions that carry it. The capture the PUSCH
# processor writes (OCUDU_UL_DUMP) holds the resource grid and the WHOLE PDU of every reception, so
# those receptions can be replayed afterwards through either depth-3 arm -- same IQ, same PDU, same
# decoder -- with probe/attach_ab.py.
#
# WHAT IT DOES
#   1. refuses if the capture directory is missing/occupied, or if the leg label already has a log
#   2. starts `run_leg.sh gpu <label>` with OCUDU_UL_DUMP exported, receiver backend = classic
#   3. waits for `=== gNB started ===`, then tells you to put the phone through an attach
#   4. watches the leg's log for the attach's own markers and, the moment the PDU session is up,
#      waits TAIL_S more seconds and stops the gNB with ONE SIGINT (the contract run_leg.sh documents)
#   5. reports ATTACH OK / ATTACH FAILED and the number of captured receptions
#
# Run it as root, because it signals the gNB that run_leg.sh started as root:
#   sudo -E bash doc_chinese/llr_ai_detection/probe/capture_attach.sh attach-1019a
#
# Then replay (offline, no radio -- see probe/attach_ab.py):
#   python3 doc_chinese/llr_ai_detection/probe/attach_ab.py \
#     --captures doc_chinese/llr_ai_detection/work_tmp/corpus/attach/<label> \
#     --model doc_chinese/llr_ai_detection/work_tmp/data/models/d3_modcond_ft.mlmodelc
set -u -o pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
RUN_LEG=$ROOT/doc_chinese/phy_pipeline_gpu/wip/run_leg.sh
CORPUS=$ROOT/doc_chinese/llr_ai_detection/work_tmp/corpus/attach

LABEL=${1:-}
if [ -z "$LABEL" ]; then
  echo "usage: sudo -E bash $0 <leg-label>   (env: CAPTURE_DIR LEG_LOGDIR CONFIG BUDGET TAIL_S TIMEOUT_S)" >&2
  exit 2
fi
if [ "$(id -u)" != 0 ]; then
  echo "run me as root: sudo -E bash $0 $LABEL   (this script signals the gNB run_leg.sh starts)" >&2
  exit 2
fi

CONFIG=${CONFIG:-$ROOT/configs/gnb_rf_b200_tdd_n78_20mhz.yml}
CAPTURE_DIR=${CAPTURE_DIR:-$CORPUS/$LABEL}
LEG_LOGDIR=${LEG_LOGDIR:-$ROOT/doc_chinese/llr_ai_detection/work_tmp/legs/attach}
# How many receptions to keep. The attach itself costs ~365 receptions over ~4 s (measured on the
# 2026-10-17 22:50 leg: PRACH -> PDU session up in 3.7 s, 365 PUSCH receptions, 173 of them crc=KO
# retransmissions), so 1500 covers the attach plus ~20 s of the traffic that follows it -- enough to
# see the first grants the scheduler hands out after the attach, at ~137 KB per capture (~200 MB).
BUDGET=${BUDGET:-1500}
TAIL_S=${TAIL_S:-5}
TIMEOUT_S=${TIMEOUT_S:-240}

if [ ! -f "$RUN_LEG" ]; then echo "missing $RUN_LEG" >&2; exit 2; fi
if [ ! -f "$CONFIG" ]; then echo "CONFIG=$CONFIG is not a file" >&2; exit 2; fi
mkdir -p "$CAPTURE_DIR" "$LEG_LOGDIR"     # fopen does not create directories, and the capture says so once
if [ -n "$(ls -A "$CAPTURE_DIR" 2>/dev/null)" ]; then
  echo "REFUSING: $CAPTURE_DIR is not empty - point CAPTURE_DIR at a fresh directory (or move the old one)." >&2
  exit 2
fi
# A label names ONE leg (the same rule preflight_check.sh enforces): a reused label makes the poll
# below read the PREVIOUS leg's log, where the attach markers are already present -- so it would report
# ATTACH OK from a leg that just started, and stop the gNB before the phone ever attached.
if ls "$LEG_LOGDIR"/gnb_gpu_"$LABEL"_*.log >/dev/null 2>&1; then
  echo "REFUSING: the label '$LABEL' already has a leg in $LEG_LOGDIR - use a new label." >&2
  exit 2
fi
mkdir -p "$CORPUS" 2>/dev/null || true

LEG_PID=""
STDOUT_LOG=""
LOG=""
stop_leg() {
  [ -n "$LEG_PID" ] || return 0
  kill -INT "$LEG_PID" 2>/dev/null || true
  for _ in $(seq 1 600); do
    kill -0 "$LEG_PID" 2>/dev/null || break
    sleep 0.1
  done
  LEG_PID=""
}
trap 'echo; echo "stopping the leg (Ctrl-C once, as the runbook says)..."; stop_leg; exit 130' INT TERM

echo "== attach capture: label=$LABEL"
echo "   config   : $CONFIG"
echo "   capture  : $CAPTURE_DIR/C_<slot>_<rnti>{.txt,.bin}  (budget $BUDGET receptions)"
echo "   logs     : $LEG_LOGDIR/gnb_gpu_${LABEL}_*.log"
echo "   receiver : classic (the arm this capture is recorded in)"
echo

# No nested sudo: this script already IS root, and every process between it and run_leg.sh's INT trap is
# one more thing a signal has to survive. run_leg.sh keeps SUDO_USER from the outer sudo, so its pre-flight
# build still runs as the invoking user and cannot leave root-owned objects in build/.
env "LEG_CONFIG=$CONFIG" "LEG_LOGDIR=$LEG_LOGDIR" \
    "OCUDU_UL_DUMP=$CAPTURE_DIR/C" "OCUDU_UL_DUMP_COUNT=$BUDGET" \
    bash "$RUN_LEG" gpu "$LABEL" \
      --expert_phy.pusch_receiver_backend=classic &
LEG_PID=$!

# ---- wait for the gNB, then for the phone ---------------------------------------------------------
printf 'waiting for the gNB to start'
for _ in $(seq 1 600); do
  STDOUT_LOG=$(ls -t "$LEG_LOGDIR"/gnb_gpu_"$LABEL"_*.log.stdout 2>/dev/null | head -1)
  LOG=$(ls -t "$LEG_LOGDIR"/gnb_gpu_"$LABEL"_*.log 2>/dev/null | head -1)
  if [ -n "$STDOUT_LOG" ] && grep -aq '=== gNB started ===' "$STDOUT_LOG" 2>/dev/null; then
    break
  fi
  kill -0 "$LEG_PID" 2>/dev/null || { echo; echo "the leg exited before the gNB started - see the log above." >&2; wait; exit 1; }
  printf '.'
  sleep 0.5
done
echo
if [ -z "$LOG" ]; then echo "no log file appeared in $LEG_LOGDIR" >&2; stop_leg; exit 1; fi
echo "gNB is up.  ==> PUT THE PHONE THROUGH AN ATTACH NOW (airplane mode on/off). <=="
echo

# ---- the attach's own markers ---------------------------------------------------------------------
# The full attach: RRC setup, initial context setup (security), then the PDU session. The gNB prints
# these on the CU-CP/NGAP paths, so they are the same lines a live leg is judged by.
ATTACH_OK='PDU Session Resource Setup Procedure" finished successfully'
ATTACH_MID='Initial Context Setup Procedure" finished successfully'
RRC_OK='RRC Setup Procedure" finished successfully'
elapsed=0
state=""
while [ "$elapsed" -lt "$((TIMEOUT_S * 2))" ]; do
  if grep -aq "$ATTACH_OK" "$LOG" 2>/dev/null; then state="pdu"; break; fi
  if grep -aq "$ATTACH_MID" "$LOG" 2>/dev/null; then state="context"; fi
  if grep -aq "$RRC_OK" "$LOG" 2>/dev/null && [ -z "$state" ]; then state="rrc"; fi
  kill -0 "$LEG_PID" 2>/dev/null || break
  sleep 0.5
  elapsed=$((elapsed + 1))
done

if [ "$state" = "pdu" ]; then
  echo "ATTACH OK (PDU session up) after $((elapsed / 2))s of watching - keeping ${TAIL_S}s of traffic, then stopping."
  sleep "$TAIL_S"
elif [ "$state" = "context" ]; then
  echo "attach reached the INITIAL CONTEXT but no PDU session after $((elapsed / 2))s - stopping anyway."
elif [ "$state" = "rrc" ]; then
  echo "attach reached RRC SETUP but not the context/PDU session after $((elapsed / 2))s - stopping anyway."
else
  echo "NO ATTACH MARKER after $((elapsed / 2))s - stopping."
fi
stop_leg

# ---- what landed ----------------------------------------------------------------------------------
COUNT=$(ls "$CAPTURE_DIR" 2>/dev/null | grep -cE '^C_[0-9]+_[0-9]+\.txt$' || true)
SIZE=$(du -sh "$CAPTURE_DIR" 2>/dev/null | cut -f1)
RECEPTIONS=0
KO=0
if [ -n "$LOG" ] && [ -f "$LOG" ]; then
  RECEPTIONS=$(grep -acE 'PUSCH: rnti=' "$LOG" || true)
  KO=$(grep -aE 'PUSCH: rnti=' "$LOG" | grep -c 'crc=KO' || true)
fi
echo
echo "=============================================================="
echo " captured receptions : $COUNT  ($SIZE)   budget was $BUDGET"
echo " leg's PUSCH lines   : $RECEPTIONS   of which crc=KO $KO"
echo " log                 : $LOG"
if [ "$state" = "pdu" ] && [ "$COUNT" -gt 0 ]; then
  echo " VERDICT: ATTACH OK - this capture is usable."
  echo
  echo " replay it (serial; runs the two arms on the same grids):"
  echo "   python3 $ROOT/doc_chinese/llr_ai_detection/probe/attach_ab.py \\"
  echo "     --captures $CAPTURE_DIR \\"
  echo "     --model $ROOT/doc_chinese/llr_ai_detection/work_tmp/data/models/d3_modcond_ft.mlmodelc"
else
  echo " VERDICT: NOT USABLE - move it aside and capture again:"
  echo "   mv $CAPTURE_DIR ${CAPTURE_DIR}_failed_\$(date +%H%M)"
fi
echo "=============================================================="
