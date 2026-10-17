#!/usr/bin/env bash
# The FIVE pre-registered checks for a compressed depth-3 model (design section 19.170(d)).
#
# They are written here, after the criteria were committed and before the model exists, so the acceptance cannot be
# adjusted to fit the result. The reference is the DEPLOYED conditioned bundle d3_modcond_ft: this is a compression
# of it, so the question is "how much did we lose", not "is it a better model".
#
#   1. decisions: >= 99.5% identical crc against the deployed model on 4 x 300 captures, and ZERO captures the
#      compressed model fails while the deployed one decodes (no new degradation);
#   2. head 3: NMSE <= -22 dB (deployed -25..-27) and power ratio within +-0.5 dB;
#   3. head 4: error <= 2.0 dB over 12..30 dB (deployed +0.26..+1.22);
#   4. criterion 1 (McNemar exact + Newcombe) still MET on the 256QAM corpus;
#   5. latency: warm forward p50 <= 0.60 ms and arm total <= 0.75 ms (min of 3, fast mode), and still on the ANE
#      (net values identical under `all` and `cpu_ne`).
#
# Usage: verify_compact.sh <compact.mlmodelc> <compact.keras> <tag>
#   Run check 5 only when the machine is otherwise idle: a training run or a build pollutes it completely.
set -u
W=/Users/jiachengwang/dev/ocudu/doc_chinese/llr_ai_detection/work_tmp
AI=/Users/jiachengwang/dev/ocudu/lib/phy/upper/signal_processors/channel_estimator/metal/ai_train
PY=$HOME/ai_ce_work/venv/bin/python
TOOL=/Users/jiachengwang/dev/ocudu/build/lib/phy/upper/channel_processors/metal/ul_chain_replay
COMPACT=$1
KERAS=$2
TAG=$3
# N caps the captures per modulation. It exists so the SCRIPT can be dry-run against the deployed bundle (a model
# compared with itself must PASS every check) before the compressed one exists; the real run uses all 300.
N=${N:-0}
REPORT=$W/tmp/verify_$TAG.report
exec > >(tee "$REPORT") 2>&1
echo "=== $(date '+%F %T') verifying $TAG"
echo "    bundle $COMPACT"
echo "    checkpoint $KERAS"

echo
echo "=== check 1: decisions against the deployed model (4 x 300 captures)"
for qm in qpsk 16qam 64qam 256qam; do
  list="$W/tmp/deployed_list_$qm.txt"
  if [ "$N" -gt 0 ]; then head -"$N" "$list" > "$W/tmp/verify_list_$qm.txt"; list="$W/tmp/verify_list_$qm.txt"; fi
  nice -n 15 $PY "$W/probe/compare_ai_arm.py" --tool "$TOOL" --corpus "$W/corpus/aillr_${qm}_ai_s1" \
    --model "$COMPACT" --list "$list" \
    --out "$W/tmp/compact_${TAG}_$qm.csv" --work "$W/tmp/work_compact_${TAG}_$qm" --clean \
    | grep -a "captures:"
  $PY - "$W/tmp/compact_${TAG}_$qm.csv" "$W/tmp/deployed_cond_$qm.csv" "$qm" <<'PYEOF'
import csv, sys
def load(path):
    with open(path) as handle:
        return {row["capture"]: row for row in csv.DictReader(handle)}
new, old = load(sys.argv[1]), load(sys.argv[2])
shared = [k for k in new if k in old and not new[k].get("degraded") and not old[k].get("degraded")]
same = sum(1 for k in shared if new[k]["crc_ai"] == old[k]["crc_ai"])
lost = [k for k in shared if old[k]["crc_ai"] == "OK" and new[k]["crc_ai"] != "OK"]   # the cell that fails the gate
won = [k for k in shared if new[k]["crc_ai"] == "OK" and old[k]["crc_ai"] != "OK"]
newly_degraded = sum(1 for k in new if new[k].get("degraded") and not old.get(k, {}).get("degraded"))
ok = len(shared) > 0 and 100.0 * same / len(shared) >= 99.5 and len(lost) == 0
print(f"    {sys.argv[3]:>6}: comparable {len(shared)}, identical {same} ({100.0*same/max(len(shared),1):.2f}%), "
      f"deployed-only {len(lost)}, compact-only {len(won)}, newly degraded {newly_degraded} -> "
      f"{'PASS' if ok else 'FAIL'}")
if lost:
    print(f"      deployed decodes and compact does not: {', '.join(sorted(lost)[:8])}")
PYEOF
done

echo
echo "=== checks 2 and 3: head 3 (channel) and head 4 (noise) against known truth"
$PY "$AI/check_reporting_heads.py" --model "$KERAS" --data "$W/data/datasets/d3_modcond_val.npz" \
  --cond --per-modulation 64 --layout spatial

echo
echo "=== check 4: criterion 1 on the 256QAM corpus (paired McNemar + Newcombe, +1 pp bound)"
$PY "$W/probe/criterion1.py" --csv "$W/tmp/compact_${TAG}_256qam.csv" --label "$TAG"

echo
echo "=== check 5: latency and placement (machine must be idle)"
CAP=$W/corpus/aillr_256qam_ai_s1/C_7_17925
best=""
for run in 1 2 3; do
  out=$(bash "$W/probe/soak_timing.sh" "$CAP" 700 "$COMPACT" "verify_${TAG}_$run" 2>&1)
  line=$(echo "$out" | grep '500 forward' | tail -1)
  fwd=$(echo "$line" | grep -o 'forward p50 [0-9]*' | awk '{print $3}')
  plane=$(echo "$line" | grep -o 'plane p50 [0-9]*' | awk '{print $3}')
  deliv=$(echo "$line" | grep -o 'deliver p50 [0-9]*' | awk '{print $3}')
  total=$(( (plane + fwd + deliv) ))
  echo "    run $run: plane $plane + forward $fwd + deliver $deliv = $total us"
  if [ -z "$best" ] || [ "$fwd" -lt "${best%% *}" ]; then best="$fwd $total"; fi
done
echo "    best: forward ${best%% *} us, arm ${best##* } us (limits 600 / 750 us)"
vals=""
for tier in all cpu_ne; do
  OCUDU_DEPTH3_COMPUTE_UNITS=$tier bash "$W/probe/soak_timing.sh" "$CAP" 3 "$COMPACT" "verify_${TAG}_pl_$tier" > /dev/null 2>&1
  vals="$vals|$(grep -a 'depth3-measure' "$W/tmp/soak/verify_${TAG}_pl_$tier/stderr.txt" | head -1 | grep -o 'rsrp net=[^ ]*' )"
done
if [ "${vals#|}" = "${vals##*|}" ]; then echo "    placement: ANE (net values identical under all and cpu_ne)"; else echo "    placement: NOT ANE ($vals)"; fi
echo "=== $(date '+%F %T') verify $TAG DONE"
