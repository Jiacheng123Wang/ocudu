#!/usr/bin/env python3
"""Split a mixed validation set into one file per modulation.

WHY THIS IS NEEDED. The trainer stacks a batch of label planes into one tensor, which requires every sample in the batch
to have the same plane -- and the plane's width is the bits per symbol. A mixed validation set therefore cannot be used to
evaluate a per-modulation model: the first batch contains a QPSK sample and a 256-QAM sample and the stack fails, which is
exactly how the first attempt at the four per-modulation runs died:

    ValueError: all input arrays must have the same shape

The conditioned model does not have this problem: its label plane is always the conditioned stride wide, uniform across
modulations, which is why mixing is fine there and not here.

Usage:
  split_val_by_modulation.py --val <mixed.npz> --out-dir <dir> [--prefix w2_val]
Writes <prefix>_QPSK.npz, _16QAM.npz, _64QAM.npz, _256QAM.npz, each carrying the same keys and dtypes as the input.
"""
import argparse
import os
import sys

import numpy as np

MODS = {2: "QPSK", 4: "16QAM", 6: "64QAM", 8: "256QAM"}


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--val", required=True)
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--prefix", default="w2_val")
    args = ap.parse_args(argv)

    z = np.load(args.val, allow_pickle=True)
    qm = z["qm"][:]
    keys = list(z.files)
    os.makedirs(args.out_dir, exist_ok=True)
    written = []
    for value, name in sorted(MODS.items()):
        idx = np.flatnonzero(qm == value)
        if idx.size == 0:
            print(f"  {name}: 0 sample(s), skipped", file=sys.stderr)
            continue
        out = {}
        for k in keys:
            arr = z[k][idx]
            out[k] = arr
        path = os.path.join(args.out_dir, f"{args.prefix}_{name}.npz")
        np.savez(path, **out)
        written.append((name, idx.size))
        print(f"  {name}: {idx.size:,} sample(s) -> {path}")
    z.close()
    print(f"total written {sum(n for _, n in written):,} sample(s) over {len(written)} file(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
