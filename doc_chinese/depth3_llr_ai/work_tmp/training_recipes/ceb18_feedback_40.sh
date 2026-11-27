#!/bin/bash
# The decoder-feedback second pass (architecture_levers section 93, item 2), first training-side arm.
#
# WHAT IT IS. `code_loss.tf_decode_feedback` turns the first pass's LLRs into the LDPC decoder's EXTRINSIC
# information (posterior minus intrinsic), scatters it back onto the network's own [612, 14, 8] layout and feeds
# it to a SECOND forward pass as eight extra input channels. The two passes share one set of weights: pass one
# sees the feedback channels blank, exactly like the deployed model, and pass two sees what the code's own
# constraint said about each bit -- the one piece of information the demapper cannot compute itself.
#
# THE ARM. Ce+B at 18 blocks, the same recipe as the reference (248 593 parameters + 8 x 32 entry weights =
# 248 849), 40 epochs, constant 1e-3. The comparison is the code-loss arm (`ceb18_code_loss_40.sh`) and the
# plain Ce+B e40 (11 856 112 at e37-e40 SWA on the 5 000-unit ruler).
#
# KNOBS. `--decode-feedback-planes 8` is one channel per modulation bit; `1` collapses them to the per-symbol
# mean, i.e. the memo's "17 channels" shorthand. `--decode-feedback-scale 20` is the calibrated S_llr: the
# extrinsic enters as clip(x / 20, -3, 3). `--code-loss-weight 0.1` keeps the decoder's own BCE in the objective
# as well, so the feedback is not the only path by which the code shapes the weights.
#
# WHAT IT CANNOT DO YET. The deployed chain runs the model ONCE per reception, so a model trained with feedback
# has no matching inference path. Measuring this arm on the ruler needs the two-pass replay prototype
# (first pass -> numpy min-sum -> fill the channels -> second pass); until then this is a training-side result.
#
# COST. Two forward passes and the min-sum, i.e. roughly 2.2-2.5x the step of the code-loss arm.
#
# BATCH 8, NOT 16. A 32-sample smoke at batch 16 passed and the real run then died asking for a single
# 4.04 GiB buffer on the 6 GB card. Batch 8 is the size that fits; the recipe is otherwise unchanged.
set -u
ROOT=${ROOT:-$HOME/work/ai_phy_train}
PY=${PY:-$ROOT/.venv/bin/python}
NV=$(ls -d "$ROOT"/.venv/lib/python3.12/site-packages/nvidia/*/lib 2>/dev/null | tr "\n" ":")
export LD_LIBRARY_PATH="$NV${LD_LIBRARY_PATH:-}"
export TF_GPU_ALLOCATOR=cuda_malloc_async
echo "=== decoder feedback (+8 channels), Ce+B 18 blocks, 40 epochs  $(date '+%F %T')"
cd "$ROOT" || exit 1
exec "$PY" -u "$ROOT/lib/phy/upper/ai_signal_processors/depth3/ai_train/train_depth3.py" \
  --train "$ROOT/data/datasets_train_w2v3c" \
  --val "$ROOT/data/datasets_train_w2v3c" \
  --out "$ROOT/runs/d3_w2v3c_c32b18dense_e2e-cond-norep-sw2s3-dskip-entry-se-code1e-1-fb8_e40" \
  --modulation QPSK --modcond --no-reporting-heads --c-hid 32 --blocks 18 --layout spatial \
  --dense-blocks --dense-skip-entry --se \
  --scale-init 0.03 --snr-weight 2 --snr-sigma 3 --loss-norm bits --smooth-k 0 \
  --epochs 40 --snapshot-every 1 --batch 8 --workers 6 --max-val 1000 \
  --no-val --monitor-every 10 --bler-samples 0 --select-on none \
  --seed 20261105 --lr 1e-3 \
  --code-loss-weight 0.1 --code-loss-iterations 2 --code-loss-samples 2 --code-loss-from 5 \
  --decode-feedback-planes 8 --decode-feedback-scale 20 --decode-feedback-first-weight 0.5 \
  "$@"
