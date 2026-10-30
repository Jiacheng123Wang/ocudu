#!/usr/bin/env bash
# STOP EVERY BUILD PROCESS ON THIS MACHINE, BY PROCESS ID AND NOT BY PATTERN.
#
# Every attempt to do this with pkill -f has gone wrong in one of two ways. The pattern matched the command that was running it, so the shell died and the
# builds lived; or the pattern was short enough to match something else, and the count it produced was of unrelated processes. Both happened here: a stale
# build ran for five hours, rebuilt legs belonging to another machine, and its stamp files made the batch look further along than it was.
#
# So: list what is running, take the pids from ps, kill those, and then list again until nothing is left.
set -u
# THE xargs POOL IS THE THING THAT OUTLIVES EVERYTHING. Killing the driver script leaves "xargs -P N bash -c run_one" parented to init, and it keeps
# starting one leg after another from a list that no longer exists as a file. Every past attempt to stop a batch here left one of these behind, which is how
# a build with the old thirty-five-leg list kept running for five hours beside the new one.
PATTERNS='build_parallel_211|queue_build_datasets\.sh|build_depth3_dataset\.py|dump_tb\.sh|one_unit|ul_chain_replay|xargs.*run_one|bash -c run_one'
for attempt in 1 2 3 4 5; do
  pids=$(ps ax -o pid=,command= | grep -E "$PATTERNS" | grep -v grep | grep -v stop_builds | awk '{print $1}')
  [ -z "$pids" ] && { echo "nothing running"; exit 0; }
  echo "attempt $attempt: killing $(echo "$pids" | wc -w | tr -d ' ') process(es)"
  # shellcheck disable=SC2086
  kill -9 $pids 2>/dev/null
  sleep 2
done
left=$(ps ax -o pid=,command= | grep -E "$PATTERNS" | grep -v grep | grep -v stop_builds | wc -l | tr -d ' ')
echo "still running: $left"
[ "$left" = "0" ] || ps ax -o pid=,command= | grep -E "$PATTERNS" | grep -v grep | cut -c1-90
