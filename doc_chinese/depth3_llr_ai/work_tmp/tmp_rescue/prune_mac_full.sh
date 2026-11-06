#!/usr/bin/env bash
# DELETE THE MAC'S COPIES OF ARRAYS THAT ARE ALREADY ON THE OTHER MACHINE, FILE BY FILE AND SIZE BY SIZE.
#
# The Mac's data volume is at twenty-five gigabytes free and its build is still writing. The arrays are the deliverable and they now exist on the other machine,
# so the local copies can go -- but only the ones that are demonstrably there, matched by name and by byte count. A file that fails either check stays.
set -u
SRC=/Users/jiachengwang/ai_depth3/datasets_full_w2
DEST=jwang@192.168.31.211
REMOTE='~/work/ai_phy_train/data/datasets_full_w2'
gone=0; kept=0; freed=0
ssh "$DEST" "cd $REMOTE && md5sum *.npz 2>/dev/null" > /tmp/remote_md5.txt || exit 1
echo "remote md5 lines: $(wc -l < /tmp/remote_md5.txt | tr -d ' ')"
while read -r hash name; do
  [ -f "$SRC/$name" ] || continue
  local_hash=$(md5 -q "$SRC/$name" 2>/dev/null || true)
  if [ "$local_hash" = "$hash" ]; then
    sz=$(stat -f %z "$SRC/$name" 2>/dev/null || echo 0)
    rm -f "$SRC/$name" && { gone=$((gone+1)); freed=$((freed+sz)); }
  else
    kept=$((kept+1))
  fi
done < /tmp/remote_md5.txt
echo "deleted $gone file(s), kept $kept, freed $((freed/1073741824)) G"
df -h /System/Volumes/Data | tail -1 | awk '{print "free now:", $4}'
