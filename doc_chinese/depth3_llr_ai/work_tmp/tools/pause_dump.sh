#!/usr/bin/env bash
# Pause or resume the running dump without losing work.
#
#   pause_dump.sh stop    SIGSTOP the dump tree (the loop plus every replay child)
#   pause_dump.sh go      SIGCONT them
#   pause_dump.sh pause   create <out>/PAUSE so the loop stops AFTER the current leg (clean, preferred before a flight)
#   pause_dump.sh unpause remove it
#
# SIGSTOP is the only lever that interrupts a leg mid-flight without restarting it; the staged rows of the units
# already decoded stay on disk, so resuming continues rather than repeating.
set -u
OUT=${OUT:-$HOME/ai_depth3/dump_s2_mcs}
case "${1:-}" in
  stop)    pkill -STOP -f dump_all_legs.sh; pkill -STOP -f dump_tb.sh; echo "stopped (SIGSTOP)" ;;
  go)      pkill -CONT -f dump_all_legs.sh; pkill -CONT -f dump_tb.sh; echo "resumed (SIGCONT)" ;;
  pause)   touch "$OUT/PAUSE"; echo "PAUSE set: the loop stops before the next leg" ;;
  unpause) rm -f "$OUT/PAUSE"; echo "PAUSE cleared" ;;
  *) echo "usage: pause_dump.sh {stop|go|pause|unpause}" >&2; exit 2 ;;
esac
