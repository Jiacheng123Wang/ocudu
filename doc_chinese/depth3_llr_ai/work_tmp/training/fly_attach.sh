#!/bin/bash
# Fly the gNB with one depth3 bundle and record what the phone's attach did.
#
# WHY THE BUNDLE IS AN ARGUMENT. The attach phase is the narrow-band case: the three-PRB Msg3 and the fifteen-byte
# SRB1 are where the network is weakest (measured 2026-11-05: the E4 bundle delivers 30.7% fewer bits than the
# classical chain at three PRB or less), and the point of flying is to find out whether a model lets the handset
# attach at all, not to read a throughput number. So each run names exactly one bundle and keeps its log.
#
# The radio and the core are not started here: this only runs the DU, which is the piece the model lives in.
set -u
NAME="${1:?usage: fly_attach.sh <bundle-name> [seconds]}"
SECS="${2:-90}"
ROOT="$HOME/dev/ocudu"
BUNDLE="$HOME/ai_depth3/gate_ai_models/bundles/$NAME/$NAME.mlmodelc"
CFG="${CFG:-$ROOT/configs/pinned_mcs04.yml}"
[ -d "$BUNDLE" ] || { echo "no such bundle: $BUNDLE" >&2; exit 1; }
STAMP=$(date +%Y%m%d_%H%M%S)
LOG="$HOME/ai_depth3/flight_logs/attach_${NAME}_${STAMP}.log"
BIN="$ROOT/build/apps/gnb/gnb"
printf 'model=%s\nbin_sha=%s\nconfig=%s\npipeline=npu\nstarted=%s\n' \
  "$NAME" "$(shasum -a 256 "$BIN" | cut -c1-16)" "$CFG" "$STAMP" > "$LOG"
echo "=== $NAME  $STAMP  (${SECS}s) ===" 
echo "log: $LOG"
# npu implies the AI receiver and PURE; the model is named so the run cannot fall back to the build default.
"$BIN" -c "$CFG" \
  --expert_phy.phy_pipeline npu \
  --expert_phy.pusch_receiver_model "$BUNDLE" >> "$LOG" 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null' EXIT INT TERM
sleep "$SECS"
kill $PID 2>/dev/null
wait $PID 2>/dev/null
echo "--- 绑定与模式 ---"
grep -aE "PURE mode|bound:|computes Qm|receiver=ai" "$LOG" | head -5
echo "--- 接入指标 ---"
for k in "InitialUL" "RRC Setup Procedure" "RRC Reconfiguration Procedure" "SRB1" "SRB2"; do
  n=$(grep -ac "$k" "$LOG" 2>/dev/null || true)
  printf '  %-32s %s\n' "$k" "${n:-0}"
done
echo "--- 射频 ---"
grep -a "radio sample continuity" "$LOG" | tail -1
echo "--- 臂的降级/失败（应尽量为 0）---"
grep -acE "reception FAILED|degrad" "$LOG" 2>/dev/null | xargs -I{} printf '  degradation 行数: %s\n' {}
