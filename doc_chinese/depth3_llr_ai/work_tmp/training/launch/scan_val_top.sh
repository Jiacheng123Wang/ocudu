#!/bin/bash
# SCAN THE CHECKPOINTS THAT VAL RANKS HIGHEST, NOT FIXED EPOCHS.
#
# Why by val: val is an imperfect predictor of delivered bits (we have recorded inversions), but the checkpoints it
# favours are where the useful ones have always been -- seed-2's e32 was both val-best and ruler-best while e40 was
# val-eighth and ruler-terrible. Fixed epoch grids miss that structure; the val ranking concentrates the search.
#
# Stage 1 (here): probe 500, ~3 min each. Stage 2: the best few go to the 5 000-unit ruler.
set -u
R1=d3_w2v3c_c32b18dense_sc003_sw2s3_jit_e40          # seed 20261105
R2=d3_w2v3c_c32b18dense_e2e-cond-norep-sw2s3_s20261106_e40   # seed 20261106
OUT=/tmp/scan_val_top.txt
: > $OUT
probe_one() {   # host run epoch
    local host=$1 run=$2 ep=$3 tag=$4
    local f; f=$(printf "epoch_%04d_periodic.keras" "$ep")
    local dst=/tmp/scan_${tag}_e${ep}.keras
    [ -f "$dst" ] || scp -q -o ConnectTimeout=20 "jwang@$host:work/ai_phy_train/runs/$run/snapshots/$f" "$dst" || { echo "$tag e$ep FETCH_FAIL" >> $OUT; return; }
    local log=/tmp/scan_${tag}_e${ep}.log
    NAME="scan_${tag}_e${ep}" nice -n 10 ~/dev/ocudu/doc_chinese/depth3_llr_ai/work_tmp/measurement/probe_snapshot.sh "$dst" 500 > "$log" 2>&1
    local line; line=$(grep -E "units 500  OK" "$log" | tail -1)
    echo "$tag e$ep $line" >> $OUT
}
for e in 20 27 39 38 24; do probe_one 192.168.0.102 $R1 $e s1; done
for e in 32 24 18 16 37; do probe_one 192.168.0.121 $R2 $e s2; done
echo SCAN_DONE >> $OUT
