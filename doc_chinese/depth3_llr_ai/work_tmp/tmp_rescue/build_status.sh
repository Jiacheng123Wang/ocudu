#!/usr/bin/env bash
T=/Users/jiachengwang/ai_depth3
date '+%F %T'
echo "--- procs ---"
ps -eo pid,etime,rss,command | grep -E 'worker2\.sh|queue_build|build_depth3' | grep -v grep \
  | awk '{printf "%s %s rss=%.1fGB %s\n", $1, $2, $3/1048576, $4}'
echo "--- disk ---"
df -h "$T" | tail -1
echo "--- swap ---"
sysctl -n vm.swapusage
echo "--- stamps ---"
ls "$T"/datasets_full_w2/.done_* 2>/dev/null | xargs -n1 basename 2>/dev/null || echo none
echo "--- npz ---"
ls -lh "$T"/datasets_full_w2/*.npz 2>/dev/null || echo none
echo "--- log tail ---"
tail -30 "$T/build_hq.log"
