#!/bin/bash
# Ce+B at EIGHTEEN blocks, e41..e120 with a LONG cosine decay (1e-3 -> 1e-4) -- the "longer decay" lever.
#
# WHY THIS RUN. Every decayed arm so far decayed only the SECOND half: the e80 checkpoint was continued with a
# cosine over e81..e120 (Ce120d at 18 blocks = 11 940 720, +3.24%) while e1..e80 had run at a constant 1e-3.
# The memo lists "a longer decay (60-80 epochs)" as the one lever besides length that still pays and has never
# been measured (architecture_levers_beyond_width sections 73, 88, 94(c): decay is +0.38 pp on Ce and +0.57..0.68
# pp on Ce+B, the largest single lever). This run decays from e41 to e120 -- seventy-nine epochs of cosine --
# starting from the same machine's own e40 checkpoint of the Ce+B arm.
#
# WHAT IT WOULD BUY. If an 18-block model with a long decay reaches the 36-block 80-epoch model (11 961 632),
# depth is unnecessary at the deployment level and the MACs are halved. If it lands near the existing decayed
# arms, the decay's value is in its last forty epochs, not its position on the trajectory.
#
# HOW THE RESUME WORKS. --resume reads <out>/depth3_last.keras + <out>/resume.json, so the directory was seeded
# from the parent's e40 snapshot:
#   SRC=runs/...-dskip-entry-se_e80_fixed
#   cp -p $SRC/snapshots/epoch_0040_periodic.keras $NEW/depth3_last.keras
#   cp -p $SRC/snapshots/epoch_0040_periodic.json  $NEW/resume.json
# Expected banner: "next epoch 41, ... 40 earlier epoch(s) of history carried over", "parameters 248593",
# "lr 0.001 (cosine, from epoch 41)".
#
# MACHINE. 126 is a GTX 1070 (Pascal): .venv216 is TF 2.16.1 + cuDNN 8.9.7, because cuDNN 9 has no sm_61
# kernels. The trainer here is the same file the parent ran (md5 66025e21..., 2026-11-23), so parent and child
# are the same code.
#
# ETA: 80 epochs x ~27 min = ~36 h on the 1070. The e77-e80 tail (first 40 epochs, ~18 h) is itself a usable
# comparison against the constant-rate Ce+B e77-e80 (11 871 320).
set -u
NV=$(ls -d "$HOME"/work/ai_phy_train/.venv216/lib/python3.12/site-packages/nvidia/*/lib 2>/dev/null | tr "\n" ":")
export LD_LIBRARY_PATH="$NV${LD_LIBRARY_PATH:-}"
export TF_GPU_ALLOCATOR=cuda_malloc_async
echo "=== c32b18dense dskip-entry se, cosine 1e-3->1e-4 e41..e120  $(date +%F" "%T)"
cd "$HOME/work/ai_phy_train" || exit 1
exec .venv216/bin/python -u "$HOME/work/ai_phy_train/lib/phy/upper/ai_signal_processors/depth3/ai_train/train_depth3.py" \
  --train "$HOME/work/ai_phy_train/data/datasets_train_w2v3c" \
  --val "$HOME/work/ai_phy_train/data/datasets_monitor_1102a" \
  --out "$HOME/work/ai_phy_train/runs/d3_w2v3c_c32b18dense_e2e-cond-norep-sw2s3-dskip-entry-se_e120_decay41" \
  --modulation QPSK --modcond --no-reporting-heads --c-hid 32 --blocks 18 --layout spatial \
  --dense-blocks --dense-skip-entry --se \
  --scale-init 0.03 --snr-weight 2 --snr-sigma 3 --loss-norm bits --smooth-k 0 \
  --epochs 120 --snapshot-every 1 --batch 16 --workers 1 --max-val 1000 \
  --no-val --monitor-every 10 --bler-samples 0 --select-on none \
  --seed 20261105 --lr 1e-3 \
  --lr-decay cosine --lr-min 1e-4 --lr-decay-from 41 \
  --resume
