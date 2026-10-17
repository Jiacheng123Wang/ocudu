#!/usr/bin/env bash
# Cross-corpus transfer test of the published-SINR calibration: the coefficients were fitted on one capture session
# (aillr_256qam_ai_s1), and this measures a DIFFERENT session of the same cell configuration (aillr_256qam_classic_s1).
# Both bundles run on the same captures, so the before/after pair is matched capture by capture.
set -u
W=/Users/jiachengwang/dev/ocudu/doc_chinese/llr_ai_detection/work_tmp
PY=$HOME/ai_ce_work/venv/bin/python
TOOL=/Users/jiachengwang/dev/ocudu/build/lib/phy/upper/channel_processors/metal/ul_chain_replay
for m in d3_modcond_ft d3_modcond_cal; do
  echo "=== $m on the other session"
  nice -n 15 $PY "$W/probe/compare_ai_arm.py" --tool "$TOOL" --corpus "$W/corpus/aillr_256qam_classic_s1" \
    --model "$W/data/models/$m.mlmodelc" --list "$W/tmp/xfer_list_256qam.txt" \
    --out "$W/tmp/xfer_$m.csv" --work "$W/tmp/work_xfer_$m" --clean | grep -a "captures:"
done
echo "XFER DONE"
