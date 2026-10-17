#!/usr/bin/env bash
# The utility flight: three arms, mirrored order, on a cell whose MCS is FREE.
#
# What it is for. The objective's utility criterion is closed-loop throughput under the same channel conditions, and
# the three arms are:
#
#   a  the AI arm with the deployed bundle            (the current default: raw publication)
#   b  the AI arm with the calibrated bundle          (section 19.172: the publication matches the classical chain's)
#   c  the classical chain                            (backend=classic: the reference the scheduler has always used)
#
# Verdict (pre-registered, high-level document section 8.8): B >= A for the calibration, B >= C for the AI arm.
#
# ORDER IS MIRRORED -- a b c c b a -- and not one pass of three: the channel drifts, so each arm appears once in each
# half and the drift cancels. `flight_verdict.py` compares means over the two legs of each arm.
#
# THE ONE GUARD THAT MATTERS MOST: the cell config must NOT pin the UL MCS. The collect configuration does
# (`min_ue_mcs = max_ue_mcs = 20`, to make the corpus decodable), and with the MCS pinned the published SINR has no
# downstream at all -- `calculate_ul_mcs` clamps to [20, 20] -- so a calibration of the published value would show
# EXACTLY zero throughput difference and the flight would prove nothing (design section 19.176). This script refuses
# that configuration instead of producing a null result that looks like evidence.
#
# WHAT IT NEEDS: a UE attached and answering ping, and the operator running the traffic (iperf3 -R -t 120, with NO -b,
# and ping -i 0.2 -c 200) during each leg. The throughput numbers are the operator's; the script produces the gNB side.
#
# Usage: fly_utility_abba.sh <batch>            e.g. fly_utility_abba.sh r9
#        UE_IP=<address> to skip the ping probe's default guess; LEG_SECONDS for the operator's own timing.
set -u
BATCH=${1:-}
W=/Users/jiachengwang/dev/ocudu/doc_chinese/llr_ai_detection/work_tmp
ROOT=/Users/jiachengwang/dev/ocudu
RUNNER=$ROOT/doc_chinese/phy_pipeline_gpu/wip/run_leg.sh
DEPLOYED=${DEPLOYED:-$W/data/models/d3_modcond_ft.mlmodelc}
CALIBRATED=${CALIBRATED:-$W/data/models/d3_modcond_cal.mlmodelc}
# THE DELIVERY-SHAPED CELL: MCS 0-28 with OLLA in the loop, n78 / 20 MHz / 30 kHz / 51 PRB (612 subcarriers), 256QAM
# MCS table by default. The AI bundle's geometry declaration matches this cell, which was checked offline on
# 2026-10-17: four modulations bound, contract=modcond, and the calibration note printed.
CELL_CONFIG=${CELL_CONFIG:-$ROOT/configs/gnb_rf_b200_tdd_n78_20mhz.yml}

die() { echo "REFUSING: $*"; exit 2; }
[ -n "$BATCH" ] || die "no batch name; usage: fly_utility_abba.sh <batch> (e.g. r9)"
[ -d "$DEPLOYED" ] || die "no deployed bundle at $DEPLOYED"
[ -d "$CALIBRATED" ] || die "no calibrated bundle at $CALIBRATED"
[ -f "$CELL_CONFIG" ] || die "no cell config at $CELL_CONFIG"
pgrep -x gnb >/dev/null && die "a gNB is already running: stop it first"
grep -q "band: 78" "$CELL_CONFIG" || die "$CELL_CONFIG is not the n78 cell this model was trained on"
grep -q "channel_bandwidth_MHz: 20" "$CELL_CONFIG" || die "$CELL_CONFIG is not 20 MHz"
# The guard this script exists for.
if grep -qE '^\s*(min_ue_mcs|max_ue_mcs)\s*:' "$CELL_CONFIG"; then
  die "$CELL_CONFIG pins the UL MCS ($(grep -oE '(min|max)_ue_mcs: [0-9]+' "$CELL_CONFIG" | tr '\n' ' ')).
       With the MCS pinned the published SINR never reaches the scheduler, so the calibration cannot show any effect
       and this flight would measure nothing (design section 19.176). Use the delivery-shaped config, e.g.
       $ROOT/configs/gnb_rf_b200_tdd_n78_20mhz.yml."
fi
if ! ping -c 1 -W 2000 "${UE_IP:-192.168.1.2}" >/dev/null 2>&1; then
  die "the UE at ${UE_IP:-192.168.1.2} does not answer ping: attach it, or pass UE_IP=<address>"
fi

export LEG_CONFIG="$CELL_CONFIG"
export LEG_LOGDIR=$W/legs/$BATCH
OUT=$W/tmp/$BATCH
mkdir -p "$LEG_LOGDIR" "$OUT"
echo "=== batch $BATCH: cell $CELL_CONFIG (MCS free), logs $LEG_LOGDIR"
echo "    a = AI arm, deployed bundle   $DEPLOYED"
echo "    b = AI arm, calibrated bundle $CALIBRATED"
echo "    c = classical chain"

# Mirrored order: each arm once per half, so a drift over the six legs lands on all three equally.
# Leg names carry an occurrence number per arm (`r9a1`, `r9a2`, ...), which is what `flight_verdict.py` groups by.
declare -A seen=()
for arm in a b c c b a; do
  seen[$arm]=$(( ${seen[$arm]:-0} + 1 ))
  name="${BATCH}${arm}${seen[$arm]}"
  echo "=== $(date '+%T') leg $name (arm $arm)"
  case "$arm" in
    a) bash "$RUNNER" gpu "$name" --expert_phy.pusch_receiver_backend=ai \
         --expert_phy.pusch_receiver_model="$DEPLOYED" > "$OUT/$name.run" 2>&1 ;;
    b) bash "$RUNNER" gpu "$name" --expert_phy.pusch_receiver_backend=ai \
         --expert_phy.pusch_receiver_model="$CALIBRATED" > "$OUT/$name.run" 2>&1 ;;
    c) bash "$RUNNER" gpu "$name" --expert_phy.pusch_receiver_backend=classic \
         > "$OUT/$name.run" 2>&1 ;;
  esac
  log=$(grep -ao '/[^ ]*gnb_gpu_'"$name"'_[0-9]*\.log' "$OUT/$name.run" 2>/dev/null | head -1)
  [ -n "$log" ] || log=$(ls -t "$ROOT"/doc_chinese/phy_pipeline_gpu/wip/logs/*"$name"*.log 2>/dev/null | head -1)
  if [ -z "$log" ]; then
    echo "  NO LOG for $name -- this leg measured nothing"
    continue
  fi
  stderr_log="$log.stderr"
  echo "  log $log"
  echo -n "  degradations: "; grep -ac 'DEGRADED to the classical chain' "$stderr_log" 2>/dev/null || echo 0
  # The published value is what the scheduler acts on (section 19.174); the calibration state is in the same line.
  echo -n "  published SINR: "
  grep -a 'depth3-sinr' "$stderr_log" 2>/dev/null | tail -1 | sed 's/.*PUBLISHED SINR //' || echo "(no [depth3-sinr] line)"
  echo -n "  calibration: "
  grep -a 'CALIBRATED by the bundle' "$stderr_log" 2>/dev/null | head -1 | grep -o "Qm=8 slope [0-9.]* offset [-+0-9.]*" || echo "not declared (arm a/c)"
  echo "  now: run the traffic for this leg, then Ctrl+C the gNB and start the next one"
done

echo
echo "=== all six legs done. Turn the iperf3 numbers into the verdict with:"
echo "    \$ROOT/doc_chinese/llr_ai_detection/probe/flight_verdict.py --legs $LEG_LOGDIR \\"
echo "        --iperf \"${BATCH}a1=?,${BATCH}b1=?,${BATCH}c1=?,${BATCH}c2=?,${BATCH}b2=?,${BATCH}a2=?\" --batch $BATCH"
