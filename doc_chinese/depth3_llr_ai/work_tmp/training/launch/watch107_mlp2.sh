#!/bin/bash
# Wait for the symbol-head run to finish, then start lever A on the same card.
while pgrep -f "runs/d3_w2v3c_c32b18dense_e2e-cond-norep-sw2s3-llrp-symaux_e40" >/dev/null; do sleep 120; done
sleep 60
cd "$HOME/work/ai_phy_train" && bash run107_mlp2.sh > logs/mlp2_e40.log 2>&1
