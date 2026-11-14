#!/bin/bash
# Poll Time Machine backup status every 5 minutes (max 90 min).
# Baseline (before manual backup): 27 backups, newest 2026-10-26-074446, 179Gi free.

DEST=/Volumes/lenovoHDDBuckup
LOG=/tmp/tm_monitor_loop.log
START=$(date +%s)
MAX=5400
SLICE=300
BASELINE_COUNT=27
PREV_NEWEST=2026-10-26-074446

log() { echo "[$(date '+%H:%M:%S')] $*"; }

get_running() {
  tmutil status 2>/dev/null | grep -E '^[[:space:]]*Running[[:space:]]*=' | head -1 | awk -F'[=;]' '{gsub(/ /,"",$2); print $2}'
}

snapshot() {
  echo "--- $(date '+%Y-%m-%dT%H:%M:%S%z') ---"
  echo "[status]"
  tmutil status 2>/dev/null
  echo "[df]"
  df -h "$DEST" 2>/dev/null | tail -1
  echo "[df_avail_mb] $(df -m "$DEST" 2>/dev/null | tail -1 | awk '{print $4}')"
  echo "[backup_count] $(tmutil listbackups 2>/dev/null | wc -l | tr -d ' ')"
  echo "[newest] $(tmutil listbackups 2>/dev/null | tail -1)"
}

log "===== MONITOR START ====="
snapshot

LAST_RUN=1
while true; do
  NOW=$(date +%s)
  ELAPSED=$((NOW - START))
  if [ "$ELAPSED" -ge "$MAX" ]; then
    log "TIMEOUT: 90 minutes elapsed, still running=$LAST_RUN"
    break
  fi
  log "sleeping 300s (elapsed ${ELAPSED}s)"
  sleep "$SLICE"
  ELAPSED=$(( $(date +%s) - START ))
  log "===== CHECK (elapsed ${ELAPSED}s) ====="
  snapshot
  LAST_RUN=$(get_running)
  LAST_RUN=${LAST_RUN:-0}
  log "parsed Running=$LAST_RUN"
  if [ "$LAST_RUN" = "0" ]; then
    log "Backup no longer running."
    break
  fi
done

log "===== POST-RUN ====="
snapshot

NEWEST=$(tmutil listbackups 2>/dev/null | tail -1)
COUNT=$(tmutil listbackups 2>/dev/null | wc -l | tr -d ' ')
AVAIL_MB=$(df -m "$DEST" 2>/dev/null | tail -1 | awk '{print $4}')
SUCCESS=0
case "$NEWEST" in
  *2026-10-26-074446*) SUCCESS=0 ;;
  *2026-10-3*) SUCCESS=1 ;;
  "") SUCCESS=0 ;;
  *) SUCCESS=1 ;;
esac

if [ "$LAST_RUN" = "0" ] && [ "$SUCCESS" = "0" ]; then
  log "===== FAILURE PATH: collecting diagnostics ====="
  echo "### DIAG: log show last 15m backupd"
  log show --last 15m --predicate 'process == "backupd"' 2>/dev/null | grep -iE "non-OSS|Copy stage|paramErr|error|fail|space" | tail -10 | cut -c1-200
  echo "### DIAG: listbackups tail -3"
  tmutil listbackups 2>/dev/null | tail -3
  echo "### DIAG: status"
  tmutil status 2>/dev/null
  echo "### DIAG: df"
  df -h "$DEST" 2>&1 | tail -1
fi

if [ "$SUCCESS" = "1" ]; then
  log "===== SUCCESS PATH: 10-minute thinning watch ====="
  WSTART=$(date +%s)
  for i in 1 2 3 4 5; do
    sleep 120
    echo "[thin $(date '+%H:%M:%S')] count=$(tmutil listbackups 2>/dev/null | wc -l | tr -d ' ') avail_h=$(df -h "$DEST" 2>/dev/null | tail -1 | awk '{print $4}') avail_mb=$(df -m "$DEST" 2>/dev/null | tail -1 | awk '{print $4}') running=$(get_running) newest=$(tmutil listbackups 2>/dev/null | tail -1)"
  done
fi

log "===== MONITOR END (elapsed $(( $(date +%s) - START ))s) ====="
