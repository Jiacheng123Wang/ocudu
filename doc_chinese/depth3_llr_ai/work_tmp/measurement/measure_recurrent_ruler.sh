#!/bin/bash
# Turn the recurrent run's tail snapshots (e117-e120) into the ruler-5000 number, in the project's protocol.
#
# WHAT IT DOES, IN ORDER
#   1. copies the four tail snapshots from the training host;
#   2. averages them with avg_weights.py (the 4-round equal-weight SWA the recipe names);
#   3. builds the deployment bundle: export + 8 width buckets + coremlcompiler + calibrate_depth3 sidecar;
#   4. with --measure, copies the bundle to 203, PAUSES the full-leg queue (two replays corrupt each other),
#      runs measure_arm.py on the 5 000 ruler units, and resumes the queue.
#
# WHY THE PAUSE IS IN THE SCRIPT. The runbook says two replays must never run concurrently; a ruler run started into a
# live full-leg queue corrupts both. The pause/resume pair is the measured fix, and it has to bracket the measurement
# even when the measurement fails, hence the trap.
#
# Usage:
#   RUN=<run dir on the training host> NAME=<canonical name> [--measure] [--host nuc5GC] [--epochs "117 118 119 120"]
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../../.." && pwd)
PY=${PY:-$HOME/ai_ce_work/venv/bin/python}
HOST=${HOST:-nuc5GC}
RUN=${RUN:-$HOME/work/ai_phy_train/runs/d3_w2v3c_c32b6x6rec_dense_e2e-cond-norep-sw2s3-dskip-entry-se_e120_decay}
NAME=${NAME:-d3_w2v3c_c32b6x6rec_e120d}
EPOCHS=${EPOCHS:-"117 118 119 120"}
STAGE=${STAGE:-$HOME/ai_depth3/recurrent/$NAME}
BUNDLE=$STAGE/bundle
MEASURE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --measure) MEASURE=1; shift;;
    --host) HOST=$2; shift 2;;
    --epochs) EPOCHS=$2; shift 2;;
    *) echo "unknown argument $1" >&2; exit 2;;
  esac
done

mkdir -p "$STAGE/snapshots" "$BUNDLE"
echo "=== 1/4 snapshots from $HOST:$RUN"
for epoch in $EPOCHS; do
  file=$(printf "epoch_%04d_periodic.keras" "$epoch")
  scp -q "$HOST:$RUN/snapshots/$file" "$STAGE/snapshots/$file" || { echo "missing $file" >&2; exit 1; }
done
ls -l "$STAGE/snapshots"

echo "=== 2/4 SWA e$(echo "$EPOCHS" | tr ' ' '/')"
"$PY" "$HERE/../analysis/avg_weights.py" "$STAGE/$NAME.keras" "$STAGE"/snapshots/*.keras

echo "=== 3/4 bundle (export + buckets + compile + calibrate)"
CALIB=${CALIB:-$HOME/ai_depth3/datasets_monitor_1102a}
bash "$REPO/lib/phy/upper/ai_signal_processors/depth3/ai_train/make_bucket_bundle.sh" \
  "$STAGE/$NAME.keras" --name "$NAME" --out "$BUNDLE" --calib "$CALIB" --measure 2>&1 | tail -40

if [ "$MEASURE" != "1" ]; then
  echo "=== prepared only; rerun with --measure to run the 5 000-unit ruler on 203"
  exit 0
fi

echo "=== 4/4 ruler on 203 (full-leg queue paused for the duration)"
UNITS=${UNITS:-$HOME/ai_depth3/ruler_s2_amc_5000_units.txt}
CSV=ruler_${NAME}.csv
ssh laptopM2 "mkdir -p ~/ai_depth3/gate_ai_models/bundles ~/ai_depth3/runs"
scp -q -r "$BUNDLE" "laptopM2:ai_depth3/gate_ai_models/bundles/$NAME"
ssh laptopM2 "bash ~/dev/ocudu/doc_chinese/depth3_llr_ai/work_tmp/measurement/fullleg_pause.sh; \
  trap 'bash ~/dev/ocudu/doc_chinese/depth3_llr_ai/work_tmp/measurement/fullleg_resume.sh' EXIT; \
  python3 ~/ai_depth3/measure_arm.py $UNITS $NAME/$NAME.mlmodelc ~/ai_depth3/runs/$CSV \
    > ~/ai_depth3/runs/ruler_${NAME}.log 2>&1; \
  tail -1 ~/ai_depth3/runs/ruler_${NAME}.log"
echo "=== CSV: 203:~/ai_depth3/runs/$CSV"
