#!/bin/bash
# Freeze the full-leg replay queue before a ruler run: two replays must never run concurrently (runbook section 3).
#
# WHY A PAUSE AND NOT JUST A LOW PRIORITY.  A low priority keeps the machine responsive, but concurrent
# replays CORRUPT each other's channel data (measured -36 dB SINR, sign flips) because the replay's
# capture path is shared.  The ruler takes about ten minutes, so the full-leg queue is frozen with
# SIGSTOP for the duration and resumed with SIGCONT afterwards.  The queue loses only the ruler time.
set -u
PIDS=$(pgrep -f "measure_fulllegen.py")
if [ -z "$PIDS" ]; then echo "no full-leg run"; exit 0; fi
for p in $PIDS; do kill -STOP "$p" 2>/dev/null && echo "STOP $p"; done
# The per-unit replay child is allowed to finish (it is a fraction of a second); the ruler must not
# start while any replay is alive.
for _ in $(seq 1 100); do
  if ! pgrep -f "ul_chain_replay" >/dev/null; then echo "no replay alive"; exit 0; fi
  sleep 0.1
done
echo "WARNING: a replay is still alive after 10 s"; exit 1
