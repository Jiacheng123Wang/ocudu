#!/usr/bin/env bash
# ONE leg, in the order the hardware needs: the gNB first, then the traffic.
#
# Why not a batch. A leg is a physical measurement: the phone is where it is, the gNB has to be up before the UE can
# attach, and the traffic can only start once the UE answers. Batching twelve of those hides the state of each one and
# leaves no moment to look at a leg before paying for the next. So this script flies exactly one leg and prints what it
# measured; the operator decides when to fly the next.
#
# Why the order matters. iperf3 started before the gNB is up finds no route to the phone and dies in a second -- the
# earlier version of this automation started the traffic first precisely because it ran the gNB in the foreground, and
# that is backwards.
#
# Usage:
#   UE_IP=<phone ip> bash fly_one_leg.sh r9ra1 a     # receiver contrast, arm a (the AI arm)
#   UE_IP=<phone ip> bash fly_one_leg.sh r9rc1 c     # receiver contrast, arm c (the classical chain)
#   UE_IP=<phone ip> bash fly_one_leg.sh r9cb1 b     # calibration batch, arm b (calibrated bundle)
#
# The cell is chosen from the tag by convention -- the third character of `r9ra1` is `r`, meaning the receiver-contrast
# batch on the collect cell; `r9ca1` has `c`, the calibration batch on the delivery cell -- and CELL_CONFIG overrides it.
#
# Arms: a = AI arm, deployed bundle      b = AI arm, calibrated bundle
#       c = classical chain              d = AI arm, calibrated bundle, reporting=net (R6's delivery form)
set -u
TAG=${1:-}
ARM=${2:-}
ROOT=/Users/jiachengwang/dev/ocudu
W=$ROOT/doc_chinese/llr_ai_detection/work_tmp
RUNNER=$ROOT/doc_chinese/phy_pipeline_gpu/wip/run_leg.sh
DEPLOYED=$W/data/models/d3_modcond_ft.mlmodelc
CALIBRATED=$W/data/models/d3_modcond_cal.mlmodelc
COLLECT=$ROOT/configs/gnb_rf_b200_tdd_n78_20mhz_llr_collect_p0default.yml
DELIVERY=$ROOT/configs/gnb_rf_b200_tdd_n78_20mhz.yml
BATCH=$(printf '%s' "$TAG" | sed 's/[a-d][0-9]*$//')
LOGDIR=$W/legs/$BATCH
OUT=$W/tmp/$BATCH
CN_HOST=${CN_HOST:-jwang@jwang-dn}
IPERF_SECONDS=${IPERF_SECONDS:-120}
PING_COUNT=${PING_COUNT:-200}
TRAFFIC=${TRAFFIC:-ssh}
READY_TIMEOUT=${READY_TIMEOUT:-120}

die() { echo "REFUSING: $*"; exit 2; }
case "$ARM" in a|b|c|d) ;; *) die "second argument must be one of a b c d (got '${ARM}')" ;; esac
[ -n "$TAG" ] || die "usage: fly_one_leg.sh <tag e.g. r9ra1> <arm a|b|c|d>"
[ -n "${UE_IP:-}" ] || die "pass the phone's address, UE_IP=10.45.0.x"
pgrep -x gnb >/dev/null && die "a gNB is already running: stop it first"

# The cell by the tag's convention, overridable.
mode=${TAG: -3:1}
case "$mode" in
  r) CELL_CONFIG=${CELL_CONFIG:-$COLLECT} ;;
  c) CELL_CONFIG=${CELL_CONFIG:-$DELIVERY} ;;
  *) CELL_CONFIG=${CELL_CONFIG:-$DELIVERY}; echo "note: tag '$TAG' has no phase letter; using $CELL_CONFIG" ;;
esac
pins_mcs=0
grep -qE '^\s*(min_ue_mcs|max_ue_mcs)\s*:' "$CELL_CONFIG" && pins_mcs=1
if [ "$pins_mcs" = 1 ] && { [ "$ARM" = b ] || [ "$ARM" = d ]; }; then
  die "$CELL_CONFIG pins the UL MCS, and arm $ARM tests the PUBLISHED SINR, which a pinned MCS never reaches.
       Fly the calibration arms on the delivery cell: ${TAG%?}c..."
fi
case "$ARM" in
  a|b|d) [ -d "$DEPLOYED" ] && [ -d "$CALIBRATED" ] || die "a bundle is missing under $W/data/models" ;;
esac

mkdir -p "$LOGDIR" "$OUT"
echo "=== leg $TAG, arm $ARM"
echo "    cell $CELL_CONFIG"
case "$ARM" in
  a) flags=(--expert_phy.pusch_receiver_backend=ai --expert_phy.pusch_receiver_model="$DEPLOYED") ;;
  b) flags=(--expert_phy.pusch_receiver_backend=ai --expert_phy.pusch_receiver_model="$CALIBRATED") ;;
  c) flags=(--expert_phy.pusch_receiver_backend=classic) ;;
  d) flags=(--expert_phy.pusch_receiver_backend=ai --expert_phy.pusch_receiver_model="$CALIBRATED"
            --expert_phy.pusch_reporting_source=net) ;;
esac
echo "    bundle/flags: ${flags[*]}"

# Authenticate once, before anything that blocks.
[ "$(id -u)" = 0 ] && SUDO=() || { SUDO=(sudo -E); sudo -v || die "sudo authentication failed"; }
# Heavy instrumentation out of the leg's environment (see fly_utility_abba.sh for why this matters): `sudo -E` keeps it
# otherwise, and a leftover capture knob would record every reception.
SCRUB=(env -u OCUDU_UL_DUMP -u OCUDU_UL_PHASE_SEGMENTS -u OCUDU_METAL_GPU_TIME)

# ---- 1. THE gNB FIRST, in the background so this script can start the traffic once it is ready ----------------
echo "--- starting the gNB (it logs to $LOGDIR)"
LEG_CONFIG=$CELL_CONFIG LEG_LOGDIR=$LOGDIR "$SUDO" "${SCRUB[@]}" bash "$RUNNER" gpu "$TAG" "${flags[@]}" \
  > "$OUT/$TAG.run" 2>&1 &
runner_pid=$!
cleanup() { kill -INT "$runner_pid" 2>/dev/null; }
trap cleanup INT TERM

# ---- 2. WAIT FOR READINESS: the gNB answering, and the phone reachable from the CN machine -------------------
# Ping is the criterion, not a log line: the log says the application started, while a ping from the peer side says the
# radio is up, the UE is attached and the core's tun interface routes to it -- which is exactly what the traffic needs.
waited=0
ready=0
while [ "$waited" -lt "$READY_TIMEOUT" ]; do
  if ! kill -0 "$runner_pid" 2>/dev/null; then
    echo "the runner exited before the leg became ready; its output:"
    tail -15 "$OUT/$TAG.run" | sed 's/^/  /'
    exit 2
  fi
  if ssh -o BatchMode=yes -o ConnectTimeout=4 "$CN_HOST" "ping -c 1 -W 2 $UE_IP" >/dev/null 2>&1; then
    ready=1
    break
  fi
  sleep 3
  waited=$((waited + 3))
  [ $((waited % 15)) = 0 ] && echo "    waiting for the UE to answer ping ($waited s)"
done
[ "$ready" = 1 ] || {
  echo "the UE at $UE_IP did not answer within ${READY_TIMEOUT}s; the leg is stopped without traffic"
  cleanup; wait "$runner_pid" 2>/dev/null
  exit 2
}
echo "--- ready after ${waited}s: the UE answers ping from $CN_HOST"

# ---- 3. THE TRAFFIC, now that there is something to measure --------------------------------------------------
if [ "$TRAFFIC" = ssh ]; then
  echo "--- traffic: 120 s of iperf3 upstream plus a ping sample"
  ssh -o BatchMode=yes "$CN_HOST" \
    "ping -i 0.2 -c $PING_COUNT $UE_IP; echo '--- iperf3 ---'; iperf3 -c $UE_IP -R -t $IPERF_SECONDS" \
    2>&1 | tee "$OUT/$TAG.traffic"
else
  echo "--- traffic: run it now on $CN_HOST --  ping -i 0.2 -c $PING_COUNT $UE_IP ; iperf3 -c $UE_IP -R -t $IPERF_SECONDS"
  read -r -p "    press Enter when the traffic has finished " _
fi

# ---- 4. STOP THE gNB (SIGINT, which is what its own trap and handlers expect) --------------------------------
echo "--- stopping the gNB"
cleanup
wait "$runner_pid" 2>/dev/null
trap - INT TERM

# ---- 5. WHAT THIS LEG MEASURED ------------------------------------------------------------------------------
stderr_log=$(ls -t "$LOGDIR"/*"$TAG"*.log.stderr 2>/dev/null | head -1)
echo
echo "=== leg $TAG summary"
if [ -n "$stderr_log" ]; then
  echo "  log: $stderr_log"
  echo -n "  degradations: "; grep -ac 'DEGRADED to the classical chain' "$stderr_log"
  echo -n "  published SINR: "
  grep -a 'depth3-sinr' "$stderr_log" | tail -1 | sed 's/.*PUBLISHED SINR //' || echo "(no [depth3-sinr] line)"
  echo -n "  calibration: "
  grep -a 'CALIBRATED by the bundle' "$stderr_log" | head -1 | grep -o 'Qm=8 slope [0-9.]* offset [-+0-9.]*' \
    || echo "not declared"
else
  echo "  NO LOG for $TAG -- this leg measured nothing"
fi
if [ -s "$OUT/$TAG.traffic" ]; then
  grep -a 'receiver' "$OUT/$TAG.traffic" | tail -1 | sed 's/^/  traffic: /'
fi
echo
echo "  next: fly the next leg with the same command, or read both batches with"
echo "    $ROOT/doc_chinese/llr_ai_detection/probe/flight_verdict.py --legs $W/legs/$BATCH --iperf \"...\" --batch $BATCH"
