#!/usr/bin/env python3
"""Take a deterministic subset of a depth-3 dataset, keeping every array aligned.

WHY IT EXISTS. The corpus is the memory: a hundred thousand samples is eleven gigabytes resident before TensorFlow loads, which
is more than a sixteen-gigabyte machine has. A smaller corpus is also an experiment in its own right -- how much does the model
lose when it sees forty thousand samples instead of a hundred thousand -- and the way to ask that is a slice of the SAME corpus,
not a different collection.

The subset is an even stride rather than the first N: the corpus is ordered by leg, and a prefix would be one traffic shape while
the model is supposed to describe all of them. A stride keeps every leg's share.

Usage: slice_depth3_dataset.py --src data/e17_train/depth3.npz --dst data/e17_40k.npz --samples 43902 [--offset 0]
"""
import argparse

import numpy as np


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--dst", required=True)
    ap.add_argument("--samples", type=int, required=True)
    ap.add_argument("--offset", type=int, default=0)
    args = ap.parse_args()

    src = np.load(args.src, allow_pickle=True)
    total = len(src["X"])
    take = min(args.samples, total)
    # Even stride from the offset: every leg contributes in proportion to its size, and the choice is reproducible.
    stride = max(1, total // take)
    idx = np.arange(args.offset, total, stride)[:take]
    print(f"{args.src}: {total} sample(s) -> {len(idx)} (stride {stride}, offset {args.offset})")

    out = {}
    for name in src.files:
        arr = src[name]
        if arr.ndim >= 1 and arr.shape[0] == total:
            out[name] = arr[idx]
        else:
            out[name] = arr          # per-dataset metadata, kept as is
    np.savez_compressed(args.dst, **out)
    print(f"wrote {args.dst}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
