#!/usr/bin/env bash
# Held-out verification of the published-SINR calibration (design section 19.172): the coefficients were fitted on the
# EVEN captures of each corpus, so this measures the ODD ones -- a real hold-out, not the fitting data.
set -u
W=/Users/jiachengwang/dev/ocudu/doc_chinese/llr_ai_detection/work_tmp
PY=$HOME/ai_ce_work/venv/bin/python
TOOL=/Users/jiachengwang/dev/ocudu/build/lib/phy/upper/channel_processors/metal/ul_chain_replay
for qm in qpsk 16qam 64qam 256qam; do
  echo "=== $qm (held out)"
  nice -n 15 $PY "$W/probe/compare_ai_arm.py" --tool "$TOOL" --corpus "$W/corpus/aillr_${qm}_ai_s1" \
    --model "$W/data/models/d3_modcond_cal.mlmodelc" --list "$W/tmp/holdout_list_$qm.txt" \
    --out "$W/tmp/cal_$qm.csv" --work "$W/tmp/work_cal_$qm" --clean | grep -a "captures:"
done
echo "SINR CAL VERIFY DONE"
