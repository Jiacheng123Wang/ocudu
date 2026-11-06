#!/usr/bin/env bash
# DROP STAGING DIRECTORIES THAT NO BUILD HAS TOUCHED FOR HALF AN HOUR.
#
# Each build copies its leg's captures into tb_<leg>/stage to work on them -- eleven gigabytes per leg -- and leaves the copy behind. Ten legs would fill
# the disk on their own. The mtime test keeps this safe while a build is running: a directory in active use is touched constantly.
set -u
while true; do
  for d in /Users/jiachengwang/ai_depth3/tb_*/stage; do
    [ -d "$d" ] || continue
    find "$d" -maxdepth 0 -mmin +30 -exec rm -rf {} + 2>/dev/null
  done
  sleep 600
done
