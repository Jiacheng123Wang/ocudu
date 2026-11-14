#!/bin/bash
# Archive one directory per leg into its own .tar.gz, several at a time, and VERIFY each archive.
#
# WHY VERIFICATION IS NOT OPTIONAL. Two drivers racing once produced truncated tarballs whose size looked plausible;
# the only reliable check is that the number of FILES inside the archive equals the number of files under the source
# (a leg directory holds its captures at the top level plus more under logs/ and tb_live/, so "count the top level"
# is wrong by about 40%).
#
# Usage:
#   archive_legs.sh <src parent> <dest> <glob> [parallel] [--strip-prefix P]
# Example:
#   archive_legs.sh ~/ai_depth3/corpus /Volumes/CORPUS/s2_mcs_corpus_tarballs 's2_mcs*_ping_111[012]*' 12
#   archive_legs.sh ~/ai_depth3/dump_s2_mcs /Volumes/CORPUS/s2_mcs_tb_tarballs 'tb_*' 12
set -u
SRC=${1:?source parent directory}
DST=${2:?destination directory}
PAT=${3:?leg glob}
JOBS=${4:-8}
LOG="$HOME/ai_depth3/logs_archive_$(basename "$DST").log"
mkdir -p "$DST"
cd "$SRC" || exit 1
ls -d $PAT 2>/dev/null | sort > /tmp/_legs_$$.txt
echo "[$(date +%T)] $(wc -l < /tmp/_legs_$$.txt) legs from $SRC -> $DST, $JOBS parallel" >> "$LOG"
cat /tmp/_legs_$$.txt | xargs -P "$JOBS" -n 1 bash -c '
  SRC="'"$SRC"'"; DST="'"$DST"'"; LOG="'"$LOG"'"; d="$1"
  out="$DST/$d.tar.gz"
  [ -s "$out.ok" ] && exit 0
  if [ -s "$out" ]; then
    want=$(find "$SRC/$d" -type f | wc -l | tr -d " "); got=$(tar tzf "$out" 2>/dev/null | wc -l | tr -d " ")
    [ "$got" -ge "$want" ] && { touch "$out.ok"; echo "[$(date +%T)] kept $d ($got entries)" >> "$LOG"; exit 0; }
    echo "[$(date +%T)] redo $d ($got < $want)" >> "$LOG"; rm -f "$out"
  fi
  t0=$(date +%s)
  if ( set -o pipefail; tar cf - -C "$SRC" "$d" | gzip -1 > "$out.tmp" ); then
    want=$(find "$SRC/$d" -type f | wc -l | tr -d " "); got=$(tar tzf "$out.tmp" 2>/dev/null | wc -l | tr -d " ")
    if [ "$got" -lt "$want" ]; then rm -f "$out.tmp"; echo "[$(date +%T)] FAILED $d ($got < $want)" >> "$LOG"; exit 1; fi
    mv "$out.tmp" "$out"; touch "$out.ok"
    echo "[$(date +%T)] done $d $(( $(stat -f%z "$out" 2>/dev/null || stat -c%s "$out") / 1048576 )) MB $(( $(date +%s) - t0 ))s entries=$got" >> "$LOG"
  else
    rm -f "$out.tmp"; echo "[$(date +%T)] FAILED $d (tar/gzip)" >> "$LOG"; exit 1
  fi' _
rm -f /tmp/_legs_$$.txt
echo "[$(date +%T)] ALL DONE: $(ls "$DST"/*.tar.gz 2>/dev/null | wc -l | tr -d ' ') archives" >> "$LOG"
