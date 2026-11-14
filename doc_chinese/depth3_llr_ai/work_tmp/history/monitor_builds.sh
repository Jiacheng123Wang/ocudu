#!/usr/bin/env bash
# Sample both builds every 5 minutes, up to 45 minutes, with early stop.
set -u
MACLOG=/Users/jiachengwang/ai_depth3/build_hq.log
MACD=/Users/jiachengwang/ai_depth3/datasets_full_w2
OUT=/tmp/build_monitor.log
: > "$OUT"
prev1_mac=-1; prev1_rmt=-1; prev2_mac=-1; prev2_rmt=-1
for i in $(seq 0 9); do
  ts=$(date '+%Y-%m-%d %H:%M:%S')
  mac_lines=$(wc -l < "$MACLOG" | tr -d ' ')
  mac_done=$(ls "$MACD"/.done_* 2>/dev/null | wc -l | tr -d ' ')
  mac_proc=$(pgrep -f '[b]uild_depth3_dataset.py' | wc -l | tr -d ' ')
  mac_df=$(df -h /System/Volumes/Data | tail -1 | awk '{print $4" free ("$5" used)"}')
  mac_tail=$(tail -4 "$MACLOG" 2>/dev/null)
  rmt=$(ssh -o BatchMode=yes -o ConnectTimeout=10 jwang@192.168.31.211 '
    echo "TAIL"; tail -4 ~/work/ai_phy_train/logs/build_full.log 2>/dev/null
    echo "DONE"; ls ~/work/ai_phy_train/data/datasets_full_w2/.done_* 2>/dev/null | wc -l
    echo "PROCS"; pgrep -f "[b]uild_depth3_dataset.py" | wc -l
    echo "UPTIME"; uptime
    echo "LINES"; wc -l < ~/work/ai_phy_train/logs/build_full.log
  ' 2>&1) || rmt="SSH_FAILED"
  if [ "$rmt" = "SSH_FAILED" ]; then
    rmt_tail="<ssh failed>"; rmt_done="?"; rmt_proc="?"; rmt_up="?"; rmt_lines="?"
  else
    rmt_tail=$(printf '%s\n' "$rmt" | sed -n '/^TAIL$/,/^DONE$/p' | sed '1d;$d')
    rmt_done=$(printf '%s\n' "$rmt" | sed -n '/^DONE$/{n;p;}')
    rmt_proc=$(printf '%s\n' "$rmt" | sed -n '/^PROCS$/{n;p;}')
    rmt_up=$(printf '%s\n' "$rmt" | sed -n '/^UPTIME$/{n;p;}')
    rmt_lines=$(printf '%s\n' "$rmt" | sed -n '/^LINES$/{n;p;}')
  fi
  {
    echo "===== SAMPLE $i @ $ts ====="
    echo "mac: lines=$mac_lines done=$mac_done procs=$mac_proc df=$mac_df"
    printf '%s\n' "$mac_tail" | sed 's/^/  mac| /'
    echo "211: lines=$rmt_lines done=$rmt_done procs=$rmt_proc"
    printf '%s\n' "$rmt_tail" | sed 's/^/  211| /'
    echo "211 uptime: $rmt_up"
  } >> "$OUT"
  # early stop: both idle and neither log advanced for the last 10 minutes (2 samples)
  if [ "$mac_proc" = "0" ] && [ "$rmt_proc" = "0" ]; then
    if [ "$mac_lines" = "$prev1_mac" ] && [ "$prev1_mac" = "$prev2_mac" ] && \
       [ "$rmt_lines" = "$prev1_rmt" ] && [ "$prev1_rmt" = "$prev2_rmt" ]; then
      echo "EARLY_STOP: both builds idle with no new log lines for 10+ min at $ts" >> "$OUT"
      exit 0
    fi
  fi
  prev2_mac=$prev1_mac; prev2_rmt=$prev1_rmt
  prev1_mac=$mac_lines; prev1_rmt=$rmt_lines
  [ "$i" -lt 9 ] && sleep 300
done
echo "MONITOR_DONE: 45 minutes elapsed" >> "$OUT"
