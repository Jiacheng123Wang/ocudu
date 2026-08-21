#!/bin/bash
# Run "make test" from a given ctest index, detect a hung test case, kill it, and record it.
#
# Usage: scan.sh <start_index> [stall_secs] [max_test_secs]
#   stall_secs    : no new ctest output for this long -> treat current test as hung (default 120)
#   max_test_secs : a single test running longer than this -> treat as hung (default 600)
#
# Exit codes: 0 = "make test" finished on its own, 2 = hang detected and killed, 1 = usage/setup error.
set -u

# The tools live in the source tree (tests/ci/macos_triage); the run artefacts (logs, ledgers, aggregated results)
# go to <build>/macos_triage so that a clean build directory can always be regenerated without losing the tools.
TOOLS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$TOOLS_DIR/../../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build}"
TRIAGE="$BUILD_DIR/macos_triage"
LEDGER="$TRIAGE/hung_tests.tsv"
mkdir -p "$TRIAGE/logs" || exit 1

START="${1:-1}"
STALL="${2:-120}"
MAXTEST="${3:-600}"
TS=$(date +%Y%m%d-%H%M%S)
LOG="$TRIAGE/logs/run_from_${START}_${TS}.log"

cd "$BUILD_DIR" || exit 1
# Drop leftover ctest scratch logs from previously killed runs (they reach hundreds of MB).
find "$BUILD_DIR/Testing/Temporary" -name 'LastTest.log.tmp*' -mmin +1 -delete 2>/dev/null

echo "LOG=$LOG"
echo "CMD=make test ARGS=\"-I ${START},\"  (stall=${STALL}s max_test=${MAXTEST}s)"

set -m
make test ARGS="-I ${START}," > "$LOG" 2>&1 &
MAKE_PID=$!
set +m

descendants() {
  local pid=$1 kid
  for kid in $(pgrep -P "$pid" 2>/dev/null); do
    echo "$kid"
    descendants "$kid"
  done
}

last_size=-1
last_change=$(date +%s)
last_start_line=""
last_start_time=$(date +%s)

while kill -0 "$MAKE_PID" 2>/dev/null; do
  sleep 5
  now=$(date +%s)
  size=$(wc -c < "$LOG" 2>/dev/null | tr -d ' ')
  size=${size:-0}
  if [ "$size" != "$last_size" ]; then
    last_size=$size
    last_change=$now
    cur_start=$(grep -aE '^ *Start +[0-9]+:' "$LOG" | tail -1)
    if [ "$cur_start" != "$last_start_line" ]; then
      last_start_line="$cur_start"
      last_start_time=$now
    fi
  fi
  stalled=$(( now - last_change ))
  running=$(( now - last_start_time ))
  if [ "$stalled" -ge "$STALL" ] || [ "$running" -ge "$MAXTEST" ]; then
    idx=$(printf '%s' "$last_start_line" | sed -E 's/^ *Start +([0-9]+): .*/\1/')
    name=$(printf '%s' "$last_start_line" | sed -E 's/^ *Start +[0-9]+: (.*)$/\1/')
    echo "=== HANG DETECTED: stalled=${stalled}s running=${running}s ==="
    echo "HUNG_INDEX=$idx"
    echo "HUNG_NAME=$name"
    echo "--- live process snapshot ---"
    kids=$(descendants "$MAKE_PID")
    for p in $kids; do
      ps -o pid,ppid,stat,%cpu,etime,command -p "$p" 2>/dev/null | tail -n +2
    done
    echo "--- killing process tree ---"
    for p in $(printf '%s\n' $kids | tail -r); do kill -9 "$p" 2>/dev/null; done
    kill -9 -- -"$MAKE_PID" 2>/dev/null
    kill -9 "$MAKE_PID" 2>/dev/null
    wait "$MAKE_PID" 2>/dev/null
    printf '%s\t%s\t%s\tstalled=%ss\trunning=%ss\t%s\n' \
      "$idx" "$name" "$(date '+%Y-%m-%d %H:%M:%S')" "$stalled" "$running" "$(basename "$LOG")" >> "$LEDGER"
    echo "=== killed; recorded in $LEDGER ==="
    tail -5 "$LOG"
    exit 2
  fi
done

wait "$MAKE_PID"
rc=$?
echo "=== make test process exited rc=$rc ==="
tail -40 "$LOG"
echo ""
# Categorized accounting over the real total (ctest's own summary excludes Disabled from the total and folds
# Crashed into the failures). Also prints the Skipped / Disabled / Crashed lists with their reasons.
python3 "$TOOLS_DIR/postrun_summary.py" "$LOG"
exit 0
