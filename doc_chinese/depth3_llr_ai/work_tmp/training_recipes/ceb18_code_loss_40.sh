#!/bin/bash
# The code-constraint loss, first arm: Ce+B at 18 blocks, 40 epochs, weight 0.1, two min-sum iterations.
#
# WHAT IT IS. Section 93 of the architecture memo ranked this first among the levers never tried: every lever so
# far improved the prediction of the GENIE LLRs, while the receiver is judged by whether the transport block
# decodes. `code_loss.py` gathers the network's LLRs into the chain's order, descrambles, deinterleaves, deposits
# them into the codeblock's circular buffer (the same arithmetic as `ldpc_decode.rate_dematch`, called not copied)
# and runs `--code-loss-iterations` normalized min-sum passes over `nr_ldpc.build_H`; the BCE on the A-POSTERIORI
# LLRs at the transmitted positions is then added to the objective. The gradient reaches the demapper through the
# decoder, so "the block decoded" can pay even when the average LLR did not move.
#
# THE ARM IS THE Ce+B e40 REFERENCE, plus one term. Same 18 blocks / dense / --dense-skip-entry --se (248 593
# parameters), same w2v3c, sw2s3, seed 20261105, batch 16, constant 1e-3, 40 epochs. Its e37-e40 SWA scored
# 11 856 112 (+2.51%) on the 203 five-thousand-unit ruler, so the comparison is against a known number.
#
# THE KNOBS. `--code-loss-samples 2` of the sixteen carry the decoder: the term is ~1.2 M floats per sample per
# iteration, so it is a regulariser on part of each batch, not a second training loop. `--code-loss-from 5` lets
# the plain LLR fit settle before the decoder starts shaping it.
#
# HOW TO READ IT. The epoch line prints `code <value>`; it starts near ln 2 = 0.693 and should fall. The verdict is
# the ruler on the e37-e40 tail weight average, not the loss.
#
# COST, measured on the Mac CPU (512 samples, 3 epochs, 2 of 8 samples): +53%. On a GPU the CNN step is relatively
# cheaper per sample, so the fraction is smaller; the machine-side smoke in the watcher prints the real number.
#
# MACHINE NOTES.
#   211 (this script): `data/datasets_monitor_1102a` is absent there, so validation reuses the training set; with
#     --no-val --select-on none that only affects the periodic health check.
#   126: `.venv` is `.venv216`; 107/211: `.venv`.
set -u
ROOT=${ROOT:-$HOME/work/ai_phy_train}
PY=${PY:-$ROOT/.venv/bin/python}
NV=$(ls -d "$ROOT"/.venv/lib/python3.12/site-packages/nvidia/*/lib 2>/dev/null | tr "\n" ":")
export LD_LIBRARY_PATH="$NV${LD_LIBRARY_PATH:-}"
export TF_GPU_ALLOCATOR=cuda_malloc_async
echo "=== code-constraint loss, Ce+B 18 blocks, 40 epochs  $(date '+%F %T')"
cd "$ROOT" || exit 1
exec "$PY" -u "$ROOT/lib/phy/upper/ai_signal_processors/depth3/ai_train/train_depth3.py" \
  --train "$ROOT/data/datasets_train_w2v3c" \
  --val "$ROOT/data/datasets_train_w2v3c" \
  --out "$ROOT/runs/d3_w2v3c_c32b18dense_e2e-cond-norep-sw2s3-dskip-entry-se-code1e-1_e40" \
  --modulation QPSK --modcond --no-reporting-heads --c-hid 32 --blocks 18 --layout spatial \
  --dense-blocks --dense-skip-entry --se \
  --scale-init 0.03 --snr-weight 2 --snr-sigma 3 --loss-norm bits --smooth-k 0 \
  --epochs 40 --snapshot-every 1 --batch 16 --workers 6 --max-val 1000 \
  --no-val --monitor-every 10 --bler-samples 0 --select-on none \
  --seed 20261105 --lr 1e-3 \
  --code-loss-weight 0.1 --code-loss-iterations 2 --code-loss-samples 2 --code-loss-from 5 \
  "$@"
