#!/bin/bash
# Start the s3 decay continuation when the symbol-head run on this machine finishes.
while pgrep -f "symaux_e40" >/dev/null; do sleep 120; done
sleep 60
cd "$HOME/work/ai_phy_train" && bash run106_s3_decay40.sh > logs/s3_decay40.log 2>&1
