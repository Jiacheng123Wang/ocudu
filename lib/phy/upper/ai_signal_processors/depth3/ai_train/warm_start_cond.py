#!/usr/bin/env python3
"""Build the conditioned model's starting weights by averaging the trunks of the per-modulation models.

WHY AVERAGE. The trunk learns how to see a channel -- estimation, equalisation, features -- and none of that depends on
the modulation, which is why the same twenty-six thousand parameters exist in every per-modulation model and in the
conditioned one. Four models trained on four different modulations therefore hold four views of the same thing. Starting
the conditioned model from any one of them would tilt it toward that modulation's signal-to-noise range and PRB mix;
averaging keeps the modulation independence the trunk is supposed to have.

WHAT IS COPIED AND WHAT IS NOT. Measured on this graph: of ninety-six weight tensors, ninety-four have identical shapes
between a per-modulation model and the conditioned one, and two do not -- the main output head's kernel and bias, which go
from two logits per resource element to eight. Those two are left at their fresh initialisation, because the mapping from
features to per-symbol logits is precisely the part that must be learned for the wider head. Everything else, the input
convolution and all eighteen residual blocks and the two auxiliary heads, is copied as the average of the four.

Usage:
  warm_start_cond.py --out <weights.weights.h5> <a.keras> <b.keras> <c.keras> <d.keras>
Then train with --init <weights.weights.h5>.
"""
import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                 "..", "..", "..", "signal_processors",
                                                 "channel_estimator", "metal", "ai_train")))

import tensorflow as tf                                    # noqa: E402
import train_depth3 as T                                   # noqa: E402
import depth3 as D3                                        # noqa: E402

# The conditioned head's stride covers the widest modulation; the graph takes twelve input channels either way, because
# the modulation is carried by the head width and the per-sample mask, not by extra input channels (measured: entry_1x1
# is (1,1,12,32) in both graphs).
COND_QM = max(D3.COND_QM_ORDER)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True, help="where to write the averaged starting weights (.weights.h5)")
    ap.add_argument("--dtype", default="float16")
    ap.add_argument("--blocks", type=int, default=18)
    ap.add_argument("--c-hid", type=int, default=32)
    ap.add_argument("sources", nargs="+", help="the per-modulation .keras models")
    args = ap.parse_args(argv)

    dst = T.build(width=D3.NOF_SUBCARRIERS, blocks=args.blocks, qm=D3.BITS_PER_SYMBOL["256QAM"],
                  aux_heads=True, c_hid=args.c_hid, dtype=args.dtype, layout="spatial",
                  in_channels=D3.NOF_CHANNELS_SPATIAL, cond=True, reporting_heads=False)
    print(f"conditioned graph: {len(dst.weights)} weight tensor(s), "
          f"{int(sum(np.prod(w.shape) for w in dst.weights))} parameter(s)")

    acc = {w.path: [] for w in dst.weights}
    copied_paths = None
    for path in args.sources:
        src = tf.keras.models.load_model(path, compile=False)
        src_w = {w.path: np.asarray(w.numpy()) for w in src.weights}
        copied = 0
        for w in dst.weights:
            v = src_w.get(w.path)
            if v is not None and tuple(v.shape) == tuple(w.shape):
                acc[w.path].append(v)
                copied += 1
        if copied_paths is None:
            copied_paths = copied
        print(f"  {os.path.basename(path)}: copied {copied} / {len(dst.weights)} tensor(s)")
        # A DIFFERENT COUNT IS EXPECTED, NOT A FAULT. Only the widest modulation's head has the conditioned head's shape,
        # so the 256-QAM model contributes all ninety-six tensors and the others contribute ninety-four. A count below
        # ninety-four would be the real problem, so that is what is flagged.
        if copied < copied_paths - 2:
            print(f"  WARNING: {os.path.basename(path)} contributed only {copied}, "
                  f"expected at least {copied_paths - 2} -- the graphs may differ", file=sys.stderr)

    averaged, kept = [], 0
    for w in dst.weights:
        views = acc[w.path]
        if views:
            stacked = np.stack(views, axis=0).astype(np.float32)
            averaged.append(stacked.mean(axis=0).astype(np.dtype(w.dtype)))
        else:
            averaged.append(np.asarray(w.numpy()))
            kept += 1
    dst.set_weights(averaged)

    os.makedirs(os.path.dirname(os.path.abspath(args.out)) or ".", exist_ok=True)
    dst.save_weights(args.out)
    print(f"averaged {len(dst.weights) - kept} tensor(s) over {len(args.sources)} source(s); "
          f"{kept} left at their fresh initialisation")
    print(f"wrote {args.out}")
    print(f"train with: --init {args.out} --modcond")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
