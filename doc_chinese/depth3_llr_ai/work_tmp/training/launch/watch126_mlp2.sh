#!/bin/bash
# Wait for the clean batch-16 llrp run to finish, then start the stacked run on the same card.
while pgrep -f "runs/d3_w2v3c_c32b18dense_e2e-cond-norep-sw2s3-llrp_b16_e40" >/dev/null; do sleep 120; done
sleep 60
cd "$HOME/work/ai_phy_train" && bash run126_mlp2_llrp_decay.sh > logs/mlp2_llrp_decay.log 2>&1
