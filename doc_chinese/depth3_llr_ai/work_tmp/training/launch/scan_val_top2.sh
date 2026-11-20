#!/bin/bash
# Same scan as before, with a canonical NAME: probe_snapshot.sh refuses anything that does not start with d3_ so that
# every bundle in ~/ai_depth3/probe can be traced back to a run (model_naming_convention.md).
set -u
OUT=/tmp/scan_val_top2.txt
: > $OUT
probe_one() {   # host run epoch tag
    local host=$1 run=$2 ep=$3 tag=$4
    local dst=/tmp/scan_${tag}_e${ep}.keras
    if [ ! -f "$dst" ]; then
        local f; f=$(printf "epoch_%04d_periodic.keras" "$ep")
        scp -q -o ConnectTimeout=20 "jwang@$host:work/ai_phy_train/runs/$run/snapshots/$f" "$dst" || { echo "$tag e$ep FETCH_FAIL" >> $OUT; return; }
    fi
    local log=/tmp/scan2_${tag}_e${ep}.log
    NAME="d3_scan_${tag}_e${ep}" nice -n 10 ~/dev/ocudu/doc_chinese/depth3_llr_ai/work_tmp/measurement/probe_snapshot.sh "$dst" 500 > "$log" 2>&1
    echo "$tag e$ep $(grep -E 'units 500  OK' "$log" | tail -1)" >> $OUT
}
R1=d3_w2v3c_c32b18dense_sc003_sw2s3_jit_e40
R2=d3_w2v3c_c32b18dense_e2e-cond-norep-sw2s3_s20261106_e40
for e in 20 27 39 38 24; do probe_one 192.168.0.102 $R1 $e s1; done
for e in 32 24 18 16 37; do probe_one 192.168.0.121 $R2 $e s2; done
echo SCAN_DONE >> $OUT
