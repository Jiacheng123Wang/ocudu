#!/bin/bash
# Run a gtest filter with a hard timeout; on timeout, sample the stacks and kill.
# Usage: gtest_timeout.sh <binary> <filter> [secs]
set -u
BIN="${1:?binary}"
FILTER="${2:?filter}"
SECS="${3:-20}"
LOG="/tmp/gtest_timeout_$(basename "$BIN").log"

"$BIN" --gtest_filter="$FILTER" > "$LOG" 2>&1 &
PID=$!
start=$(date +%s)
while kill -0 "$PID" 2>/dev/null; do
  sleep 1
  if [ $(( $(date +%s) - start )) -ge "$SECS" ]; then
    echo "=== TIMEOUT after ${SECS}s: $FILTER ==="
    tail -6 "$LOG"
    echo "--- stacks (filtered) ---"
    sample "$PID" 1 2>/dev/null | grep -E "Thread_|usrsctp|sctp_|ocudu::|__psynch|kevent|read|write|poll" | head -40
    kill -9 "$PID" 2>/dev/null
    wait "$PID" 2>/dev/null
    exit 2
  fi
done
wait "$PID"
rc=$?
echo "=== finished rc=$rc ==="
grep -aE "^\[  (PASSED|FAILED|SKIPPED)|^\[  FAILED  \] [a-z]" "$LOG" | head -10
exit $rc
