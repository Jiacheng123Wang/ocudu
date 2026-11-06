#!/usr/bin/env bash
# PHASE 2 ONLY: select per bin and build, from the dumps the first run already produced.
#
# The first run built every dump and then selected nothing: select_by_bin.py was handed the leg name through a process substitution and read
# an empty list, so it printed "state bins: 0" twenty-two times and the build loop found no index_sel_*.csv to build from. The dumps are the
# expensive half and they are intact; this re-runs only the cheap half.
set -u
ROOT=$HOME/work/ocudu; TRAIN=$HOME/work/ai_phy_train
STATE=$TRAIN/bins_state.json
mkdir -p "$TRAIN/datasets_bin"
PY=$TRAIN/.venv/bin/python
TOOLS=$ROOT/lib/phy/upper/ai_signal_processors/depth3/ai_train
Q_SELF=2500
Q_HARQ=250
MIN_FREE_KB=$((15*1024*1024))

free_kb() { df -k "$HOME" | tail -1 | awk '{print $4}'; }

LEGS="w2_harq_mcs20_ping1_1027 w2_harq_mcs20_ping1a_1027 \
      w2_harq_mcs20_ping100_1027 w2_harq_mcs20_ping100a_1027 w2_harq_mcs20_ping100b_1027 \
      w2_harq_mcs12_ping500_1027 w2_harq_mcs12_ping500a_1027 \
      w2_harq_mcs06_ping500_1027 w2_harq_mcs06_ping500a_1027 \
      w2_harq_iperf_udp1M_1027 w2_harq_iperf_udp_1027 \
      w2_harq_iperf_udp3M_1027 w2_harq_iperf_udp3Ma_1027 w2_harq_iperf_udp3Mb_1027 \
      w2_harq_iperf_udp3Mc_1027 w2_harq_iperf_udp3Md_1027 \
      w2_harq_ping1400_1027 w2_harq_ping500_1027 w2_harq_ping500_1027a w2_harq_ping500_1027b \
      w2_harq_ping500_1027c w2_harq_1027"

echo "===== phase2 $(date +%T) free=$(df -h $HOME | tail -1 | awk '{print $4}')"
for LEG in $LEGS; do
  L=$TRAIN/corpus/$LEG; O=$TRAIN/tb_$LEG
  [ -s "$O/index.csv" ] || { echo "no dump: $LEG"; continue; }
  if [ "$(free_kb)" -lt "$MIN_FREE_KB" ]; then
    echo "STOP: free space below the guard"; break
  fi
  "$PY" "$TOOLS/select_by_bin.py" --legs "$LEG" --corpus "$TRAIN/corpus" --tb-root "$TRAIN" \
      --state "$STATE" --quota-self $Q_SELF --quota-harq $Q_HARQ 2>&1 | grep -E "selected|state|REFUSING" | tail -3
  for pop in self harq; do
    F=$O/index_sel_$pop.csv
    [ -s "$F" ] || continue
    for qm in QPSK 16QAM 64QAM 256QAM; do
      [ "$(free_kb)" -lt "$MIN_FREE_KB" ] && { echo "STOP (guard)"; break 3; }
      "$PY" "$TOOLS/build_depth3_dataset.py" --units "$F" --tb "$O/tb" --corpus "$L" \
          --out "$TRAIN/datasets_bin" --name "${LEG}_${qm}_${pop}" --source real --modulation "$qm" \
          --min-prb 1 --max-units 0 2>&1 | tail -1
    done
  done
  echo "---- built $LEG $(date +%T) free=$(df -h $HOME | tail -1 | awk '{print $4}')"
done
echo "===== done $(date +%T)"
