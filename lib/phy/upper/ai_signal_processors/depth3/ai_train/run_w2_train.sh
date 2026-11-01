#!/usr/bin/env bash
# The w2 training run on the laptop.
#
# THE MODEL SHAPE IS E4/E17 VERBATIM -- eighteen blocks, thirty-two channels, twenty-six thousand one hundred and
# eighty-five parameters, spatial layout, twelve input channels, modulation-conditioned, no reporting heads. The only
# variable this run introduces is the corpus, which is the point: the w2 set carries a deliberate harq share that the
# earlier corpora did not.
#
# TWO THINGS ARE DIFFERENT FROM A DESKTOP RUN, both forced by this machine.
#   * --max-train 40000. The full corpus is about eighteen gigabytes of arrays and the machine has fifteen of memory; a
#     forty-thousand cap is about five. It also lands beside E4's forty-three thousand nine hundred and two, which makes
#     the comparison cleaner rather than muddier.
#   * The CUDA libraries live inside the virtual environment, so their directories have to be on the loader path or
#     TensorFlow silently runs on the CPU -- five times slower and with no error.
#
# --patience is explicit because the default is zero, which means no early stopping at all.
set -u
cd "$HOME/work/ai_phy_train" || exit 1

NV=$(ls -d .venv/lib/python*/site-packages/nvidia/*/lib 2>/dev/null | tr "\n" ":")
export LD_LIBRARY_PATH="$NV${LD_LIBRARY_PATH:-}"

exec ./.venv/bin/python -u \
  "$HOME/work/ocudu/lib/phy/upper/ai_signal_processors/depth3/ai_train/train_depth3.py" \
  --train data/w2_train_mixed.npz \
  --val data/w2_monitor1.npz \
  --out runs/w2_c32_b18 \
  --modcond --layout spatial --blocks 18 --c-hid 32 --in-channels 12 \
  --no-reporting-heads --w-ch 0.1 --w-noise 0.3 \
  --batch 8 --epochs 60 --patience 3 \
  --max-train 40000 --max-val 1000 \
  --bler-every 5 --bler-samples 8 \
  --resume
