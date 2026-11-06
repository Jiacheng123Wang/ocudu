#!/usr/bin/env bash
# ARCHIVE EACH LEG INSTEAD OF COPYING ITS FILES. Copying the corpus file by file ran at 4.8 MB/s -- twenty gigabytes in seventy minutes -- while
# a single 500 MB write to the same disk ran at 88 MB/s. The difference is per-file metadata on exFAT: a hundred thousand small files cost far
# more than their bytes. One sequential stream per leg runs at disk speed.
set -u
C=/Users/jiachengwang/dev/ocudu/doc_chinese/llr_ai_detection/work_tmp/corpus
D=/Volumes/CORPUS/tarballs
mkdir -p "$D"
for leg in $(ls -d "$C"/w2_* | xargs -n1 basename); do
  [ -f "$D/$leg.tar" ] && { echo "have $leg"; continue; }
  echo "---- $leg $(date +%T)"
  tar -cf "$D/$leg.tar" -C "$C" "$leg" || echo "FAILED $leg"
  echo "     done $(date +%T)  $(du -h "$D/$leg.tar" | cut -f1)"
done
echo "===== all archived $(date +%T)"
