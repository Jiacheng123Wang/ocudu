#!/usr/bin/env bash
# SEND THE BUILT ARRAYS TO THE MACHINE THAT HOLDS THE MOTHER SET.
#
# The Mac's data volume is at ninety-seven percent: the product itself is forty-seven gigabytes, the dumps twenty-eight, and the captures the rest. The
# arrays are the deliverable and the other machine has the room, so they travel now rather than waiting for the last four legs.
#
# One tar of the whole directory: these are large files, so the wire runs at its full rate, unlike the capture trees where every file is a few kilobytes.
set -u
SRC=/Users/jiachengwang/ai_depth3/datasets_full_w2
DEST=jwang@192.168.31.211
TRAIN='~/work/ai_phy_train'
TAR=/tmp/full_w2_mac.tar

echo "===== packing $(date +%T) ($(du -sh $SRC | cut -f1))"
tar -cf "$TAR" -C "$(dirname $SRC)" "$(basename $SRC)" || exit 1
echo "===== sending $(date +%T) ($(du -h $TAR | cut -f1))"
scp -q "$TAR" "$DEST:/tmp/full_w2_mac.tar" || { echo "scp failed"; exit 1; }
echo "===== extracting on 211 $(date +%T)"
ssh "$DEST" "tar -xf /tmp/full_w2_mac.tar -C $TRAIN/data/ && rm -f /tmp/full_w2_mac.tar && ls $TRAIN/data/datasets_full_w2/*.npz | wc -l"
rm -f "$TAR"
echo "===== done $(date +%T)"
