#!/usr/bin/env bash
# SEND THE FOUR NEW SIXTEEN-QAM LEGS AND BUILD THEM, DETACHED ON THE HONG KONG MACHINE.
#
# One tar per leg over the LAN, then a build launched under flock so it cannot collide with any other builder. Everything after the transfer runs
# without this session.
set -u
C=/Users/jiachengwang/dev/ocudu/doc_chinese/llr_ai_detection/work_tmp/corpus
DEST=jwang@192.168.31.211
TRAIN='~/work/ai_phy_train'
LEGS="w2_harq_16QAM_ping_1030a w2_harq_16QAM_ping_1030b w2_harq_16QAM_ping_1030c w2_harq_16QAM_ping_1030d"

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

echo "===== build (detached, flock) $(date +%T)"
ssh "$DEST" "cd $TRAIN && setsid nohup flock -n /tmp/ocudu_chain.lock bash -c '
  echo \"===== 16QAM 1030 batch \$(date +%T)\" >> build_qpsk.log
  TRAIN=\$HOME/work/ai_phy_train \$HOME/work/ocudu/lib/phy/upper/ai_signal_processors/depth3/ai_train/queue_build_datasets.sh $LEGS >> build_qpsk.log 2>&1
  echo \"===== 16QAM 1030 batch done \$(date +%T)\" >> build_qpsk.log
' > /dev/null 2>&1 < /dev/null & echo launched"
echo "===== done $(date +%T)"
