#!/bin/bash
# Resume the full-leg replay queue after a ruler run.
set -u
PIDS=$(pgrep -f "measure_fulllegen.py")
if [ -z "$PIDS" ]; then echo "no full-leg run"; exit 0; fi
for p in $PIDS; do kill -CONT "$p" 2>/dev/null && echo "CONT $p"; done
