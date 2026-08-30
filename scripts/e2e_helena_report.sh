#!/usr/bin/env bash
# G-3 (20 MHz ZMQ E2E) one-shot acceptance report from /tmp/gnb.log.
# Run after the E2E (gnb must have run with OCUDU_MMSE_TIME=1 and the log
# level >= debug so the [helena_time] lines are present):
#   sudo OCUDU_MMSE_TIME=1 ./build/apps/gnb/gnb -c configs/gnb_zmq_oaiue.yaml \
#       expert_phy --pusch_ldpc_decoder_type auto
#   scripts/e2e_helena_report.sh [logfile]
set -u
LOG=${1:-/tmp/gnb.log}
[ -f "$LOG" ] || { echo "no log: $LOG"; exit 1; }

echo "== binary =="
grep -m1 "Built in" "$LOG"

echo "== Core ML model loads (expect 30 = 10 estimators x 3 engines) =="
grep -c "Core ML model loaded" "$LOG"

echo "== engine buckets used =="
grep -o "engine=[0-9]*" "$LOG" | sort | uniq -c | sort -rn

echo "== first 106-bucket prediction (the spike check: predict must be ~<1 ms) =="
grep -m1 "engine=1272" "$LOG"

echo "== all helena predictions: mean/max (us) =="
grep -o "predict=[0-9.]*us" "$LOG" | sed 's/predict=//;s/us//' \
  | awk '{s+=$1; n++; if($1>m) m=$1} END {printf "n=%d mean=%.0f max=%.0f\n", n, s/n, m}'

echo "== pipeline probes (stop-time summary) =="
grep -E "\[ul_(pipeline|time_frequency|channel_estimation|equalization_demod|ldpc_decode|mac_pdu_size)\]" "$LOG" | tail -7

echo "== ALERT lines (should be empty or benign) =="
grep -iE "overflow|missed deadline|late slot|too late" "$LOG" | tail -5
