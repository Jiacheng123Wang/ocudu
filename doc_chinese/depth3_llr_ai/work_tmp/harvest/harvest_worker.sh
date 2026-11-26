#!/bin/bash
# Run one leg's harvest end to end on a worker: fetch the archives, harvest, push the result, clean up.
#
# THE WORKER NEVER KEEPS A LEG. The corpus tar is 1-3 GB and the extracted captures are another 3-4 GB; a worker that
# keeps them fills its disk after a handful of legs. So the order is fixed -- fetch, harvest, upload, delete -- and the
# upload is verified against the local file sizes before anything is removed.
#
# Usage: harvest_worker.sh <leg> <kind: amc|mcs|w2> [--jobs 10] [--cap 20]
set -u
LEG=$1; KIND=$2; shift 2
JOBS=10; CAP=20
while [ $# -gt 0 ]; do
  case "$1" in
    --jobs) JOBS=$2; shift 2 ;;
    --cap) CAP=$2; shift 2 ;;
    *) echo "unknown option $1"; exit 2 ;;
  esac
done
SRC=192.168.31.211
case "$KIND" in
  amc) KINDUR=s2_amc ;;
  mcs) KINDUR=s2_mcs ;;
  w2)  KINDUR=w2 ;;
  *) echo "unknown kind $KIND"; exit 2 ;;
esac
WORK=$HOME/harvest_work
mkdir -p "$WORK"
TB="$WORK/${LEG}_tb.tar.gz"; CORP="$WORK/${LEG}.tar.gz"

echo "=== $LEG ($KIND) $(date +%H:%M:%S)"
[ -s "$TB" ] || scp -q jwang@$SRC:/mnt/corpus/${KINDUR}_tb_tarballs/tb_${LEG}.tar.gz "$TB"
[ -s "$TB" ] || { echo "ABORT: no tb tarball for $LEG"; exit 1; }
[ -s "$CORP" ] || scp -q jwang@$SRC:/mnt/corpus/${KINDUR}_corpus_tarballs/${LEG}.tar.gz "$CORP"
[ -s "$CORP" ] || { echo "ABORT: no corpus tarball for $LEG"; exit 1; }

bash "$HOME/work/ai_phy_train/harvest_leg.sh" "$LEG" "$TB" "$CORP" "$WORK" --jobs "$JOBS" --cap "$CAP" || exit 1

# Upload, then verify by size before deleting the local copies.
ssh -o BatchMode=yes jwang@$SRC "mkdir -p /mnt/corpus/harvest_hard/$LEG/tb"
scp -q "$WORK/$LEG/index_hard.csv" jwang@$SRC:/mnt/corpus/harvest_hard/$LEG/index_hard.csv || exit 1
if [ -n "$(ls -A "$WORK/$LEG/tb" 2>/dev/null)" ]; then
  scp -q "$WORK/$LEG/tb/"*.bits jwang@$SRC:/mnt/corpus/harvest_hard/$LEG/tb/ || exit 1
fi
local_n=$(cat "$WORK/$LEG/tb/"*.bits 2>/dev/null | wc -c)
remote_n=$(ssh -o BatchMode=yes jwang@$SRC "cat /mnt/corpus/harvest_hard/$LEG/tb/*.bits 2>/dev/null | wc -c")
echo "  bytes local=$local_n remote=$remote_n"
[ "$local_n" = "$remote_n" ] || { echo "ABORT: upload mismatch, keeping local files"; exit 1; }

rm -rf "$WORK/$LEG" "$TB" "$CORP"
echo "=== $LEG done $(date +%H:%M:%S), local temp removed; disk: $(df -h "$HOME" | awk 'NR==2{print $4}') free"
