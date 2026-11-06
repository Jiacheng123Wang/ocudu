#!/usr/bin/env bash
# TRANSFER EVERY 16-QAM LEG, THEN CHAIN ITS BUILD BEHIND THE BULK BUILD AND THE 1029h BUILD.
#
# One waiter, not three: the bulk build owns the machine's cores for hours, and two waiters that poll every minute can wake at the same instant and start
# two dumps on eight cores. So this script sends the data, then installs a single detached chain that waits for the bulk build, builds 1029h, and builds
# this batch -- in that order, one after another.
set -u
C=/Users/jiachengwang/dev/ocudu/doc_chinese/llr_ai_detection/work_tmp/corpus
DEST=jwang@192.168.31.211
TRAIN='~/work/ai_phy_train'
HARQ=w2_harq_QPSK_ping_1029h
LEGS="w2_harq_16QAM_ping_1029a w2_harq_16QAM_ping_1029b w2_harq_16QAM_ping_1029c w2_harq_16QAM_ping_1029d \
      w2_harq_16QAM_ping_1029e w2_harq_16QAM_ping_1029f w2_harq_16QAM_ping_1029g \
      w2_harq_mcs06_ping_1029 w2_harq_mcs06_ping_1029a"

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

echo "===== installing the single build chain $(date +%T)"
TOOLS='$HOME/work/ocudu/lib/phy/upper/ai_signal_processors/depth3/ai_train/queue_build_datasets.sh'
ssh "$DEST" "cd $TRAIN && setsid nohup bash -c '
  while pgrep -f queue_build_datasets >/dev/null 2>&1; do sleep 60; done
  echo \"===== chained: $HARQ \$(date +%T)\" >> build_qpsk.log
  TRAIN=\$HOME/work/ai_phy_train $TOOLS $HARQ >> build_qpsk.log 2>&1
  echo \"===== chained: 16QAM batch \$(date +%T)\" >> build_qpsk.log
  TRAIN=\$HOME/work/ai_phy_train $TOOLS $LEGS >> build_qpsk.log 2>&1
  echo \"===== chained: all done \$(date +%T)\" >> build_qpsk.log
' > /dev/null 2>&1 < /dev/null & echo installed"
echo "===== done $(date +%T)"
