#!/usr/bin/env bash
# PUSH THE MAC'S QPSK LEGS TO 211 AND START THE BUILD THERE.
#
# One tar per leg, over the LAN: the corpus is a hundred and fifty gigabytes in two hundred thousand small files, and file-by-file copying over
# USB to exFAT ran at five megabytes a second -- a tar stream ran at a hundred and ten. The remote side extracts into the same tree the build
# expects, then the build runs detached so it survives this session.
set -u
C=/Users/jiachengwang/dev/ocudu/doc_chinese/llr_ai_detection/work_tmp/corpus
DEST=jwang@192.168.31.211
TRAIN='~/work/ai_phy_train'
LEGS="w2_harq_mcs02_ping_1028 w2_harq_mcs02_ping_1028a w2_harq_mcs02_ping_1028b w2_harq_mcs02_ping_1028c \
      w2_harq_mcs02_ping_1028d w2_harq_mcs02_ping_1028de w2_harq_mcs02_ping_1028e w2_harq_mcs02_ping_1028f \
      w2_harq_mcs02_ping_1028g w2_harq_QPSK_iperf_1029c w2_harq_QPSK_iperf_1029d w2_harq_QPSK_ping_1029e \
      w2_harq_QPSK_ping_1029f w2_harq_QPSK_ping_clip_1029g"

echo "===== transfer $(date +%T)"
for leg in $LEGS; do
  [ -d "$C/$leg" ] || { echo "absent $leg"; continue; }
  if ssh "$DEST" "test -d $TRAIN/corpus/$leg"; then echo "have $leg"; continue; fi
  echo "---- $leg ($(du -sh "$C/$leg" | cut -f1)) $(date +%T)"
  if ! tar -cf "/tmp/push_$leg.tar" -C "$C" "$leg"; then echo "tar failed $leg"; continue; fi
  if ! scp -q "/tmp/push_$leg.tar" "$DEST:/tmp/push_$leg.tar"; then echo "scp failed $leg"; rm -f "/tmp/push_$leg.tar"; continue; fi
  ssh "$DEST" "tar -xf /tmp/push_$leg.tar -C $TRAIN/corpus/ && rm -f /tmp/push_$leg.tar"
  rm -f "/tmp/push_$leg.tar"
  echo "     on 211: $(ssh "$DEST" "find $TRAIN/corpus/$leg -maxdepth 1 -name 'cap_*.txt' ! -name '*_ce.txt' | wc -l")   $(date +%T)"
done

echo "===== build (detached on 211) $(date +%T)"
ssh "$DEST" "cd $TRAIN && setsid nohup ./queue_build_datasets.sh $LEGS > build_qpsk.log 2>&1 < /dev/null & echo launched"
echo "===== done $(date +%T)"
