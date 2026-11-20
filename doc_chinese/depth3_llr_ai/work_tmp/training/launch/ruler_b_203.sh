#!/bin/bash
# Ruler for the two best checkpoints of lever B (LLR-plane post-filter), ON 203 because the Mac is collecting gNB corpus.
set -u
cd $HOME
for tag in llrp_e32 llrp_e39; do
  python3 $HOME/ai_depth3/measure_arm.py $HOME/ai_depth3/ruler_s2_amc_5000_units.txt \
      $tag/$tag.mlmodelc $HOME/ai_depth3/runs/ruler_${tag}_203.csv > $HOME/ai_depth3/runs/ruler_${tag}_203.log 2>&1
done
echo RULER_B_DONE
