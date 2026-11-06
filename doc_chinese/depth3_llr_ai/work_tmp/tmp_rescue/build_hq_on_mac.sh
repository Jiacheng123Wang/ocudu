#!/usr/bin/env bash
# BUILD THE 64QAM AND 256QAM LEGS ON THE MAC, WHERE THEIR CAPTURES ALREADY ARE, INSTEAD OF SHIPPING THE GRIDS.
#
# The alternative was to push a hundred and ninety-seven gigabytes of captures over the wire and build them on the other machine. The captures are here, the
# toolchain is here, and the product -- one npz per leg and modulation -- is about a tenth the size. So build here and send the product.
#
# Everything is nice'd: this machine still flies the gNB, and a build must not make a capture late.
set -u
TRAIN=/Users/jiachengwang/ai_depth3
TOOLS=/Users/jiachengwang/dev/ocudu/lib/phy/upper/ai_signal_processors/depth3/ai_train
PY=/Users/jiachengwang/ai_ce_work/venv/bin/python
LOG=$TRAIN/build_hq.log
LEGS="w2_harq_64QAM_ping_1029a w2_harq_64QAM_ping_1029b w2_harq_64QAM_ping_1029c \
      w2_harq_64QAM_ping_1030a w2_harq_64QAM_ping_1030b \
      w2_harq_256QAM_ping_1030a w2_harq_256QAM_ping_1030b w2_harq_256QAM_ping_1030e \
      w2_harq_256QAM_iperf_1030c w2_harq_256QAM_iperf_1030d \
      w2_harq_mcs20_ping_1028"

for leg in $LEGS; do
  if [ -s "$TRAIN/tb_$leg/index.csv" ] && ls "$TRAIN/datasets_full_w2/${leg}"_*.npz > /dev/null 2>&1; then
    echo "have $leg" >> "$LOG"
    continue
  fi
  echo "---- $leg $(date +%T)" >> "$LOG"
  TRAIN=$TRAIN DATASETS_DIR=$TRAIN/datasets_full_w2 STATE=$TRAIN/bins_state_full.json \
  Q_SELF=100000000 Q_HARQ=100000000 PY=$PY JOBS=10 \
  nice -n 10 "$TOOLS/queue_build_datasets.sh" "$leg" >> "$LOG" 2>&1
  echo "     done: $(ls "$TRAIN/datasets_full_w2/${leg}"_*.npz 2>/dev/null | wc -l) npz $(date +%T)" >> "$LOG"
done
echo "===== all done $(date +%T)" >> "$LOG"
