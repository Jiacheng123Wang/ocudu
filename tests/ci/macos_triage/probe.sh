#!/bin/bash
# Re-run a single ctest test with a hard wall-clock cap, to tell a true hang from a merely slow test.
#
# Usage: probe.sh <exact_test_name> [cap_secs]
# Exit codes: 0 = test finished within the cap (see printed result), 2 = still running at the cap (hang), 1 = setup error.
set -u

TOOLS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$TOOLS_DIR/../../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build}"
TRIAGE="$BUILD_DIR/macos_triage"
mkdir -p "$TRIAGE/logs" || exit 1

NAME="${1:?usage: probe.sh <exact_test_name> [cap_secs]}"
CAP="${2:-420}"
SAFE=$(printf '%s' "$NAME" | tr -c 'A-Za-z0-9._-' '_')
LOG="$TRIAGE/logs/probe_${SAFE}.log"

cd "$BUILD_DIR" || exit 1
echo "PROBE=$NAME cap=${CAP}s log=$LOG"

# ctest -R takes a regex; anchor it so only the exact test name matches.
ESC=$(printf '%s' "$NAME" | sed -E 's/[][\.^$*+?(){}|]/\\&/g')

set -m
ctest -R "^${ESC}$" --output-on-failure --timeout "$CAP" > "$LOG" 2>&1 &
PID=$!
set +m

descendants() {
  local pid=$1 kid
  for kid in $(pgrep -P "$pid" 2>/dev/null); do
    echo "$kid"
    descendants "$kid"
  done
}

start=$(date +%s)
while kill -0 "$PID" 2>/dev/null; do
  sleep 5
  if [ $(( $(date +%s) - start )) -ge $(( CAP + 30 )) ]; then
    echo "=== STILL RUNNING after $(( $(date +%s) - start ))s -> HANG ==="
    kids=$(descendants "$PID")
    for p in $kids; do ps -o pid,ppid,stat,%cpu,etime,command -p "$p" 2>/dev/null | tail -n +2; done
    for p in $(printf '%s\n' $kids | tail -r); do kill -9 "$p" 2>/dev/null; done
    kill -9 -- -"$PID" 2>/dev/null
    kill -9 "$PID" 2>/dev/null
    wait "$PID" 2>/dev/null
    exit 2
  fi
done
wait "$PID"
rc=$?
echo "=== ctest exited rc=$rc after $(( $(date +%s) - start ))s ==="
grep -aE 'Passed|Failed|Timeout|Subprocess|tests passed|Total Test time' "$LOG" | tail -20
exit 0
