#!/usr/bin/env bash
# The six capture gates, one command (the durable copy of the /tmp runner: /tmp does not survive a
# reboot, and losing the runner is how a session ends up verifying nothing).
#
# usage: bash run_gates.sh [output-dir] [corpus-glob] [jobs]
#   corpus-glob defaults to the synthetic corpus (/tmp/corpus/*.bin, see make_synthetic_capture.py).
#   The recorded corpus (/tmp/iq1_*_ce.txt ...) is the stronger one when it exists: its pinned anchors
#   pin the host's cross-session behaviour, which a synthetic corpus cannot.
set -u
OUT=${1:-/tmp/gates_run}
GLOB=${2:-$( [ -d /Users/jiachengwang/dev/ocudu/doc_chinese/work_tmp/corpus ] && echo /Users/jiachengwang/dev/ocudu/doc_chinese/work_tmp/corpus || echo /tmp/corpus )/*.bin}
JOBS=${3:-6}
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
G=$HERE/../../../lib/phy/upper/signal_processors/channel_estimator/metal/capture_gates.sh
mkdir -p "$OUT"
rc_all=0
for m in sig2 k0dm k0d ydev ratdev k1 combos; do
  echo "=== $m start $(date +%H:%M:%S)"
  bash "$G" "$m" "$JOBS" "$GLOB" > "$OUT/$m.log" 2>&1
  rc=$?
  echo "=== $m rc=$rc $(date +%H:%M:%S)"
  tail -4 "$OUT/$m.log"
  [ "$rc" -eq 0 ] || rc_all=1
done
echo "ALL_GATES_RC=$rc_all"
exit $rc_all
