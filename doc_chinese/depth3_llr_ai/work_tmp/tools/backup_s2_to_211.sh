#!/bin/bash
# Back up the s2 capture corpus and its dump to 211, one tar per leg.
#
# WHY PER LEG. A single 440 GB tar cannot be inspected, resumed or partially restored; one archive per leg
# can be listed, re-made on its own, and matched against the leg it came from.
#
# WHY THE BYTE COUNT. The stream is counted locally with tee while it is being sent, and the size of the
# file that lands on 211 is compared against that count. That catches a truncated transfer without reading
# 440 GB twice, which on a USB drive would double the wall time for nothing.
set -u
HOST=jwang@192.168.31.211
CORPUS=/Volumes/T7_Corpus/corpus
DUMP=/Users/jiachengwang/ai_depth3/dump_s2
LOG=/Users/jiachengwang/ai_depth3/logs_backup
DEST_C=/mnt/corpus/s2_corpus_tarballs
DEST_D=/mnt/corpus/s2_tb_tarballs
mkdir -p "$LOG"
log() { echo "$(date '+%F %T') $*" >> "$LOG/driver.log"; }

send() {   # send <src-parent> <leaf> <dest-dir>
  local parent=$1 leaf=$2 dest=$3 t0 local_bytes remote_bytes
  t0=$(date +%s)
  tar -C "$parent" -cf - "$leaf" 2>>"$LOG/errors.log" \
    | tee >(wc -c > "$LOG/.bytes") \
    | ssh -o BatchMode=yes "$HOST" "cat > $dest/$leaf.tar.tmp && mv $dest/$leaf.tar.tmp $dest/$leaf.tar" 2>>"$LOG/errors.log"
  local rc=$?
  sleep 2
  # BSD wc pads its count to a fixed width when it reads a stream, so the local side arrives as
  # " 10392688640" and a string comparison against stat's bare digits calls every leg FAILED -- which
  # is exactly what the first run logged while the sizes were equal. Strip the padding.
  local_bytes=$(tr -d ' \n' < "$LOG/.bytes" 2>/dev/null || echo 0)
  remote_bytes=$(ssh -o BatchMode=yes "$HOST" "stat -c %s $dest/$leaf.tar 2>/dev/null" || echo -1)
  if [ "$rc" -ne 0 ] || [ "$local_bytes" != "$remote_bytes" ]; then
    log "FAILED $leaf rc=$rc local=$local_bytes remote=$remote_bytes"
    return 1
  fi
  log "ok $leaf $(( $(date +%s) - t0 ))s $local_bytes bytes"
}

log "=== start corpus from $CORPUS ($(ls "$CORPUS" | wc -l | tr -d ' ') entries)"
for leg in $(ls "$CORPUS"); do
  [ -d "$CORPUS/$leg" ] || continue
  send "$CORPUS" "$leg" "$DEST_C"
done

log "=== start dump from $DUMP ($(ls "$DUMP" | wc -l | tr -d ' ') entries)"
for leg in $(ls "$DUMP"); do
  [ -d "$DUMP/$leg" ] || continue
  send "$DUMP" "$leg" "$DEST_D"
done

log "=== done; corpus tars: $(ssh -o BatchMode=yes "$HOST" "ls $DEST_C/*.tar 2>/dev/null | wc -l") dump tars: $(ssh -o BatchMode=yes "$HOST" "ls $DEST_D/*.tar 2>/dev/null | wc -l")"
log "=== remote usage: $(ssh -o BatchMode=yes "$HOST" "du -sh $DEST_C $DEST_D | tr '\n' ' '")"
