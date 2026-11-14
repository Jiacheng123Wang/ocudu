#!/usr/bin/env bash
# SEND THE SIX 64QAM AND 256QAM LEGS, THEN BUILD THEM -- AFTER THE ARCHIVE HAS FREED THE DISK.
#
# Ordered behind the archive on purpose: these six legs are two hundred and sixteen gigabytes, the machine has a hundred and seventy free, and the archive
# that runs first frees about a hundred and fifty more. Waiting for its flag costs time nobody is spending, and avoids a transfer that fills the disk.
set -u
C=/Users/jiachengwang/dev/ocudu/doc_chinese/llr_ai_detection/work_tmp/corpus
DEST=jwang@192.168.31.211
TRAIN='~/work/ai_phy_train'
LEGS="w2_harq_64QAM_ping_1029a w2_harq_64QAM_ping_1029b w2_harq_64QAM_ping_1029c \
      w2_harq_256QAM_ping_1030a w2_harq_256QAM_ping_1030b w2_harq_256QAM_ping_1030e"

echo "===== waiting for the archive flag $(date +%T)"
until ssh "$DEST" "test -f /tmp/qpsk16qam_archived"; do sleep 120; done
echo "===== archive done, free: $(ssh "$DEST" 'df -h ~ | tail -1 | cut -d" " -f7-')"

echo "===== transfer $(date +%T)"
for leg in $LEGS; do
  [ -d "$C/$leg" ] || { echo "absent $leg"; continue; }
  if ssh "$DEST" "test -d $TRAIN/corpus/$leg"; then echo "have $leg"; continue; fi
  echo "---- $leg ($(du -sh "$C/$leg" | cut -f1)) $(date +%T)"
  tar -cf "/tmp/push_$leg.tar" -C "$C" "$leg" 2>/dev/null || { echo "tar failed $leg"; continue; }
  scp -q "/tmp/push_$leg.tar" "$DEST:/tmp/push_$leg.tar" || { echo "scp failed $leg"; rm -f "/tmp/push_$leg.tar"; continue; }
  ssh "$DEST" "tar -xf /tmp/push_$leg.tar -C $TRAIN/corpus/ 2>/dev/null && rm -f /tmp/push_$leg.tar"
  rm -f "/tmp/push_$leg.tar"
  echo "     on 211: $(ssh "$DEST" "find $TRAIN/corpus/$leg -maxdepth 1 -name 'cap_*.txt' ! -name '*_ce.txt' | wc -l") captures $(date +%T)"
done

echo "===== build (detached, blocking lock) $(date +%T)"
ssh "$DEST" "cd $TRAIN && setsid nohup flock /tmp/ocudu_chain.lock bash -c '
  cd \$HOME/work/ai_phy_train
  echo \"===== 64QAM/256QAM batch \$(date +%T)\" >> logs/build_qpsk.log
  TRAIN=\$HOME/work/ai_phy_train \$HOME/work/ocudu/lib/phy/upper/ai_signal_processors/depth3/ai_train/queue_build_datasets.sh $LEGS >> logs/build_qpsk.log 2>&1
  echo \"===== 64QAM/256QAM batch done \$(date +%T)\" >> logs/build_qpsk.log
' > /dev/null 2>&1 < /dev/null & echo launched"
echo "===== done $(date +%T)"
