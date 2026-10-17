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
# A FOURTH ARM IS AVAILABLE and the order is overridable: `d` is the calibrated bundle WITH `net` reporting, i.e. what
# R6 actually delivers. Offline it publishes the same numbers as `b` to within 0.1 dB (RSRP/EPRE) and 0.4 dB (SINR),
# because under either reporting mode the arm publishes its own SINR and the difference is only where RSRP/EPRE come
# from -- so d is EXPECTED to equal b, and running it is how that expectation gets tested rather than assumed.
#
# TRAFFIC. `TRAFFIC=ssh` (the default) drives the operator's traffic from HERE over ssh: the CN machine that sits on
# the 5G core's tun interface starts `ping` and `iperf3 -c <phone> -R -t 120` for each leg, logs them next to the leg,
# and prints on the terminal when they finish so the operator knows when to Ctrl+C the gNB. `TRAFFIC=manual` leaves the
# traffic to the operator, which is what the earlier flights used.
# The reason to automate it is not convenience: every rule those legs depend on -- `-R`, no `-b`, the same two commands
# word for word -- is then applied by the script instead of remembered, and the numbers land in the batch directory
# instead of in a chat message.
#
# Usage: fly_utility_abba.sh <batch>            e.g. fly_utility_abba.sh r9
#        ARMS="a b c d d c b a" to include the R6 arm (eight legs, ~20 minutes)
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
TRAFFIC=${TRAFFIC:-manual}
CN_HOST=${CN_HOST:-jwang@jwang-dn}
IPERF_SECONDS=${IPERF_SECONDS:-120}
PING_COUNT=${PING_COUNT:-200}

die() { echo "REFUSING: $*"; exit 2; }

# THE RADIO NEEDS ROOT, and the runner is designed to be started under `sudo -E` (it uses SUDO_USER to run its
# pre-flight BUILD as the invoking user, so no root-owned objects are left in build/). The first version of this script
# called the runner directly and would have died at radio init on the first leg -- the kind of bug that costs a flight.
# Each leg is therefore elevated here while the script itself stays unprivileged; if it is already running as root the
# elevation is skipped rather than doubled.
if [ "$(id -u)" = "0" ]; then
  SUDO=()
else
  SUDO=(sudo -E)
fi

[ -n "$BATCH" ] || die "no batch name; usage: fly_utility_abba.sh <batch> (e.g. r9)"
[ -d "$DEPLOYED" ] || die "no deployed bundle at $DEPLOYED"
[ -d "$CALIBRATED" ] || die "no calibrated bundle at $CALIBRATED"
[ -f "$CELL_CONFIG" ] || die "no cell config at $CELL_CONFIG"
pgrep -x gnb >/dev/null && die "a gNB is already running: stop it first"
grep -q "band: 78" "$CELL_CONFIG" || die "$CELL_CONFIG is not the n78 cell this model was trained on"
grep -q "channel_bandwidth_MHz: 20" "$CELL_CONFIG" || die "$CELL_CONFIG is not 20 MHz"
# THE GUARD DEPENDS ON WHICH ARMS ARE RUN, and that is the whole subtlety.
#
# Arms b and d exist to test the PUBLISHED SINR (the calibration, and R6's delivery form). With the MCS pinned that
# quantity never reaches the scheduler, so those arms would measure exactly nothing -- refused (design section 19.176).
#
# Arms a and c are a different question: the AI RECEIVER against the classical one, at whatever MCS the cell is
# running. A pinned MCS is not only harmless there, it is the cleanest form of the comparison, because both arms then
# face the same code rate and the only thing that can differ is whether the reception decodes. That is why the collect
# configuration -- which pins the MCS -- is the right cell for the receiver contrast and the wrong cell for the
# calibration.
ARMS_EARLY=${ARMS:-a b c c b a}
pins_mcs=0
grep -qE '^\s*(min_ue_mcs|max_ue_mcs)\s*:' "$CELL_CONFIG" && pins_mcs=1
needs_free_mcs=0
for arm in $ARMS_EARLY; do
  case "$arm" in b|d) needs_free_mcs=1 ;; esac
done
if [ "$pins_mcs" = 1 ] && [ "$needs_free_mcs" = 1 ]; then
  die "$CELL_CONFIG pins the UL MCS ($(grep -oE '(min|max)_ue_mcs: [0-9]+' "$CELL_CONFIG" | tr '\n' ' '))
       and this run includes arm(s) b/d, which test the PUBLISHED SINR. With the MCS pinned that quantity never reaches
       the scheduler, so those arms would measure nothing (design section 19.176).
       Use the delivery-shaped config for the calibration: $ROOT/configs/gnb_rf_b200_tdd_n78_20mhz.yml
       Or run the RECEIVER contrast on this cell with ARMS=\"a c c a\" -- both arms then face the same code rate."
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
echo "    d = AI arm, calibrated bundle, reporting=net (the R6 delivery; add it with ARMS=\"a b c d d c b a\")"
if [ "$pins_mcs" = 1 ]; then
  echo "    NOTE: this cell pins the UL MCS, so the arms here measure the RECEIVER (a vs c) and not the publication."
fi

# Mirrored order: each arm once per half, so a drift over the six legs lands on all three equally.
# Leg names carry an occurrence number per arm (`r9a1`, `r9a2`, ...), which is what `flight_verdict.py` groups by.
# AUTHENTICATE ONCE, and only after every configuration guard has passed: asking for a password and then refusing the
# cell would waste the operator's attention on a batch that was never going to run. `sudo -v` refreshes the timestamp,
# and a leg takes about two and a half minutes, so the cached credential outlives the batch in practice; without it the
# operator is asked once per leg, and one that expires mid-batch stops a leg for a reason unrelated to the measurement.
if [ "$(id -u)" != "0" ]; then
  sudo -v || die "sudo authentication failed, so no leg could start the radio"
  echo "note: sudo authenticated once; the legs reuse the cached credential"
else
  echo "note: running as root, so the legs are started directly (no nested sudo)"
fi

# SCRUB THE HEAVY INSTRUMENTATION OUT OF THE LEG'S ENVIRONMENT. `sudo -E` PRESERVES the environment, and the cell config
# and the runner both leave these alone -- so an `OCUDU_UL_DUMP` left over from a corpus-collection session would make the
# flight capture EVERY reception to disk (the corpus beside this work is 74 GB; a leg would write tens of GB and could
# fill the volume mid-batch), and the phase-segment / GPU-time probes would put measurement code inside the timed path.
# They are stripped here rather than warned about, because a flight that dies on a full disk wastes the phone.
leaked=""
for variable in OCUDU_UL_DUMP OCUDU_UL_PHASE_SEGMENTS OCUDU_METAL_GPU_TIME; do
  # Indirect expansion, not `eval`: a nested eval here quoted wrong in its own test and reported every variable as set.
  value="${!variable:-}"
  [ -n "$value" ] && leaked="$leaked $variable=$value"
done
SCRUB="env -u OCUDU_UL_DUMP -u OCUDU_UL_PHASE_SEGMENTS -u OCUDU_METAL_GPU_TIME"
[ -n "$leaked" ] && echo "note: stripped from the legs' environment:$leaked"

ARMS=${ARMS:-a b c c b a}
declare -A seen=()
for arm in $ARMS; do
  seen[$arm]=$(( ${seen[$arm]:-0} + 1 ))
  name="${BATCH}${arm}${seen[$arm]}"
  echo "=== $(date '+%T') leg $name (arm $arm)"
  traffic_pid=""
  if [ "$TRAFFIC" = ssh ]; then
    # SUPERSEDED. This mode started the traffic before the runner, which is backwards -- the gNB has to be up and the
    # UE attached before iperf3 has a route. fly_one_leg.sh runs the gNB first and waits for the peer, one leg at a
    # time, which is also the granularity the hardware wants.
    die "TRAFFIC=ssh is superseded by fly_one_leg.sh, which starts the gNB first; use that script or TRAFFIC=manual"
    [ -n "${UE_IP:-}" ] || die "TRAFFIC=ssh needs UE_IP"
    # The traffic is started BEFORE the receiver: the runner blocks for the whole leg, so anything started after it
    # would only run once the leg was already over.
    ssh -o BatchMode=yes "$CN_HOST" \
      "ping -i 0.2 -c $PING_COUNT $UE_IP; echo '--- iperf3 ---'; iperf3 -c $UE_IP -R -t $IPERF_SECONDS" \
      > "$OUT/$name.traffic" 2>&1 &
    traffic_pid=$!
    # `wait` in a subshell only sees its own children, so the end is announced by polling the pid. The message goes to
    # the terminal because the runner is occupying the script's own output for the whole leg.
    ( while kill -0 "$traffic_pid" 2>/dev/null; do sleep 2; done
      printf '\n>>> traffic for %s finished -- Ctrl+C the gNB to end this leg\n' "$name" > /dev/tty 2>/dev/null ) &
    echo "    traffic started on $CN_HOST (iperf3 ${IPERF_SECONDS}s to $UE_IP)"
  else
    echo "    traffic: run it yourself now (ping -i 0.2 -c $PING_COUNT $UE_IP ; iperf3 -c $UE_IP -R -t $IPERF_SECONDS)"
  fi
  case "$arm" in
    a) "$SUDO" "$SCRUB" bash "$RUNNER" gpu "$name" --expert_phy.pusch_receiver_backend=ai \
         --expert_phy.pusch_receiver_model="$DEPLOYED" > "$OUT/$name.run" 2>&1 ;;
    b) "$SUDO" "$SCRUB" bash "$RUNNER" gpu "$name" --expert_phy.pusch_receiver_backend=ai \
         --expert_phy.pusch_receiver_model="$CALIBRATED" > "$OUT/$name.run" 2>&1 ;;
    c) "$SUDO" "$SCRUB" bash "$RUNNER" gpu "$name" --expert_phy.pusch_receiver_backend=classic \
         > "$OUT/$name.run" 2>&1 ;;
    d) "$SUDO" "$SCRUB" bash "$RUNNER" gpu "$name" --expert_phy.pusch_receiver_backend=ai \
         --expert_phy.pusch_receiver_model="$CALIBRATED" \
         --expert_phy.pusch_reporting_source=net > "$OUT/$name.run" 2>&1 ;;
  esac
  leg_rc=$?
  # A FAILED LEG ENDS THE BATCH. Carrying on would leave a batch with fewer legs than arms, and the verdict would
  # either refuse or -- worse -- compare arms that ran different numbers of times. The leg's own output is printed
  # because the reason (a refused configuration, a radio that would not open, a password never entered) is in it.
  if [ "$leg_rc" != 0 ]; then
    echo "  LEG $name EXITED $leg_rc -- stopping the batch."
    echo "  ---- last 15 line(s) of $OUT/$name.run ----"
    tail -15 "$OUT/$name.run" | sed 's/^/  /'
    echo "  The legs already flown are on disk under $LEG_LOGDIR, but a batch with a failure is not a verdict."
    exit "$leg_rc"
  fi
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
# The hint is BUILT FROM THE ARMS THAT RAN: the two-flight plan runs "a c c a" as often as the four-arm one, and a hint
# listing b1/b2 for a batch that never had them is a hint that produces an error message at the moment the operator has
# just finished the hard part.
pair_list=""
for arm in "${!seen[@]}"; do
  for index in $(seq 1 "${seen[$arm]}"); do
    pair_list="${pair_list:+$pair_list,}${BATCH}${arm}${index}=?"
  done
done
echo "=== legs done: ${!seen[*]}  (${#seen[@]} arm(s))"
echo "    $ROOT/doc_chinese/llr_ai_detection/probe/flight_verdict.py --legs $LEG_LOGDIR \\"
echo "        --iperf \"$pair_list\" --batch $BATCH"
