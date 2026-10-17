#!/usr/bin/env bash
# Decision-level capability per depth on the real 256QAM corpus, through the real chain.
set -u
W=/Users/jiachengwang/dev/ocudu/doc_chinese/llr_ai_detection/work_tmp
PY=$HOME/ai_ce_work/venv/bin/python
TOOL=/Users/jiachengwang/dev/ocudu/build/lib/phy/upper/channel_processors/metal/ul_chain_replay
for k in 18 15 12 9 6; do
  echo "=== depth $k"
  nice -n 15 $PY "$W/probe/compare_ai_arm.py" --tool "$TOOL" --corpus "$W/corpus/aillr_256qam_ai_s1" \
    --model "$W/data/models/refit_b$k.mlmodelc" --list "$W/tmp/cap_list_256qam.txt" \
    --out "$W/tmp/cap_b$k.csv" --work "$W/tmp/work_cap_b$k" --clean | grep -a "captures:"
done
echo "CAPABILITY DECODE DONE"
