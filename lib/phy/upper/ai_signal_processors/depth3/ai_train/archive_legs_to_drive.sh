#!/usr/bin/env bash
# ARCHIVE EACH LEG AS ONE TAR. Copying a corpus file by file to exFAT ran at 4.8 MB/s -- twenty gigabytes in seventy minutes -- while a single
# 500 MB write to the same disk ran at 88 MB/s. The difference is per-file metadata: a hundred thousand small files cost far more than their bytes.
# One sequential stream per leg runs at disk speed (measured ~64 MB/s, thirteen times faster).
#
# WHY .part AND A RENAME. This archives while a leg may be being written, so it can be paused or interrupted; a half-written .tar that a later
# run mistakes for a finished one is silent data loss. Each archive is written to <leg>.tar.part and renamed only after tar exits zero and the
# entry count matches the source tree.
#
# Usage: archive_legs_to_drive.sh <destination-dir> [leg ...]
#        (no legs means every w2_* directory under the corpus, except those named --exclude)
set -u
CORPUS=${CORPUS:-$(cd "$(dirname "$0")/../../../../../.." && pwd)/doc_chinese/llr_ai_detection/work_tmp/corpus}
DEST=${1:?usage: $0 <destination-dir> [leg ...]}
shift || true
EXCLUDE=${EXCLUDE:-}

mkdir -p "$DEST" || exit 2
LEGS=${*:-$(ls -d "$CORPUS"/w2_* 2>/dev/null | xargs -n1 basename)}

for leg in $LEGS; do
  for skip in $EXCLUDE; do
    [ "$leg" = "$skip" ] && { echo "skip $leg (excluded)"; continue 2; }
  done
  [ -d "$CORPUS/$leg" ] || { echo "absent $leg"; continue; }
  if [ -f "$DEST/$leg.tar" ]; then echo "have $leg"; continue; fi
  echo "---- $leg $(date +%T)"
  if tar -cf "$DEST/$leg.tar.part" -C "$CORPUS" "$leg"; then
    want=$(find "$CORPUS/$leg" | wc -l | tr -d ' ')
    got=$(tar -tf "$DEST/$leg.tar.part" 2>/dev/null | wc -l | tr -d ' ')
    if [ "$want" = "$got" ]; then
      mv "$DEST/$leg.tar.part" "$DEST/$leg.tar"
      echo "     ok $(date +%T)  $(du -h "$DEST/$leg.tar" | cut -f1)  $got entries"
    else
      echo "     INCOMPLETE: $got of $want entries, kept as .part"
    fi
  else
    echo "     tar failed, kept as .part"
  fi
done
echo "===== done $(date +%T)"
