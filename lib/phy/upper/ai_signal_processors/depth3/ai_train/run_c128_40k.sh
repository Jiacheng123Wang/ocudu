#!/usr/bin/env bash
# c128 on the forty-thousand-sample slice, on the sixteen-gigabyte Ubuntu machine.
#
# WHY THIS RUN EXISTS. It isolates corpus size at a fixed model: 221 is training c128 on a hundred thousand samples, this one
# on forty-three thousand nine hundred and two taken from the same corpus by an even stride. The difference between the two
# curves is what the missing fifty-six thousand samples are worth.
#
# The peak measured before launching was 8.9 GB, so the slice fits where the full corpus did not.
set -u
cd "$HOME/work/ai_phy_train"
PY=./.venv/bin/python
TRAIN=lib/phy/upper/ai_signal_processors/depth3/ai_train/train_depth3.py
OUT=runs/c128_e17_40k
while true; do
  "$PY" -u "$TRAIN" \
    --train data/e17_40k.npz --val data/e17_monitor/depth3.npz \
    --out "$OUT" --modcond --layout spatial --blocks 18 --c-hid 128 --in-channels 12 \
    --no-reporting-heads --w-ch 0.1 --w-noise 0.3 --batch 8 --epochs 60 \
    --patience 3 --bler-every 5 --bler-samples 8 --resume >> runs_c128_40k.log 2>&1
  rc=$?
  echo "trainer exited rc=$rc at $(date +%T)" >> runs_c128_40k.log
  [ "$rc" -eq 0 ] && break
  sleep 30
done
echo "c128_40k finished at $(date +%T)" >> runs_c128_40k.log
