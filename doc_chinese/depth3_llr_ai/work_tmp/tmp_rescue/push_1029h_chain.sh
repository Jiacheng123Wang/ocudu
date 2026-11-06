#!/usr/bin/env bash
# SEND THE HIGH-SNR LEG OVER AFTER THE BULK TRANSFER FINISHES, THEN BUILD IT AFTER THE BULK BUILD.
#
# Ordered rather than parallel on purpose: the bulk transfer already saturates the wire with tars of small files, and the build that follows it will
# own the Hong Kong machine's CPU for hours. Queuing this leg behind both costs nothing but a wait, and avoids two dumps competing for eight cores.
set -u
C=/Users/jiachengwang/dev/ocudu/doc_chinese/llr_ai_detection/work_tmp/corpus
LEG=w2_harq_QPSK_ping_1029h
DEST=jwang@192.168.31.211
TRAIN='~/work/ai_phy_train'

echo "waiting for the bulk transfer to finish $(date +%T)"
while pgrep -f push_qpsk_and_build >/dev/null 2>&1; do sleep 60; done

echo "---- transferring $LEG ($(du -sh "$C/$LEG" | cut -f1)) $(date +%T)"
tar -cf "/tmp/push_$LEG.tar" -C "$C" "$LEG" || exit 1
scp -q "/tmp/push_$LEG.tar" "$DEST:/tmp/push_$LEG.tar" || exit 1
ssh "$DEST" "tar -xf /tmp/push_$LEG.tar -C $TRAIN/corpus/ && rm -f /tmp/push_$LEG.tar"
rm -f "/tmp/push_$LEG.tar"
echo "     on 211: $(ssh "$DEST" "find $TRAIN/corpus/$LEG -maxdepth 1 -name 'cap_*.txt' ! -name '*_ce.txt' | wc -l") captures $(date +%T)"

echo "---- queueing the build behind the bulk build $(date +%T)"
ssh "$DEST" "cd $TRAIN && setsid nohup bash -c 'while pgrep -f queue_build_datasets >/dev/null 2>&1; do sleep 60; done; ./queue_build_datasets.sh $LEG >> build_qpsk.log 2>&1' > /dev/null 2>&1 < /dev/null & echo queued"
echo "===== done $(date +%T)"
