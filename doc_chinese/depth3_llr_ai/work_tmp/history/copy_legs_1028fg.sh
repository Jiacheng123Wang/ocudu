#!/usr/bin/env bash
# Copy capture tarball + tb dump for legs f/g from the capture machine.
# Captures are a plain tar stream (contains some ._ stubs, harmless).
# Dumps are excluded on the source side because macOS tar treats ._cap_* as
# AppleDouble metadata for a file that does not exist yet and fails to extract.
set -u
TRAIN=/Users/jiachengwang/ai_depth3
CORPUS=$TRAIN/corpus
REMOTE=jwang@192.168.31.211
LOG=/tmp/copy_legs_1028fg.log

echo "start $(date)" > "$LOG"
for leg in w2_harq_mcs02_ping_1028f w2_harq_mcs02_ping_1028g; do
  echo "==== capture $leg $(date +%T)" >> "$LOG"
  ssh -o BatchMode=yes "$REMOTE" "cat /mnt/corpus/tarballs/$leg.tar" | tar -xf - -C "$CORPUS" 2>>"$LOG"
  rc=$?
  n=$(find "$CORPUS/$leg" -maxdepth 1 -name 'cap_*.txt' ! -name '*_ce.txt' ! -name '._*' 2>/dev/null | wc -l | tr -d ' ')
  echo "capture rc=$rc count=$n $(date +%T)" >> "$LOG"

  echo "==== dump $leg $(date +%T)" >> "$LOG"
  set -o pipefail
  ssh -o BatchMode=yes "$REMOTE" "tar -cf - --exclude='._*' --exclude='*/._*' -C ~/work/ai_phy_train tb_$leg" | tar -xf - -C "$TRAIN" 2>>"$LOG"
  rc=$?
  set +o pipefail
  files=$(find "$TRAIN/tb_$leg" -type f 2>/dev/null | wc -l | tr -d ' ')
  stubs=$(find "$TRAIN/tb_$leg" -type f -name '._*' 2>/dev/null | wc -l | tr -d ' ')
  echo "dump rc=$rc files=$files stubs=$stubs $(date +%T)" >> "$LOG"
done
echo "ALL DONE $(date +%T)" >> "$LOG"
