#!/usr/bin/env python3
"""Convert a compressed training archive into a directory of `.npy` files that can be memory-mapped.

WHY. `Batches` reads every array eagerly, and it has to: `.npz` is a ZIP container, so nothing in it
can be memory-mapped -- even stored uncompressed, `np.load` still goes through `zipfile`, which needs
the whole member in memory. That caps the training set at what fits in RAM, which on the 15 GB machine
is about 70 000 samples out of the 160 603 the corpus holds.

Written as one `.npy` per array, the same data can be opened with `np.load(..., mmap_mode="r")` and
the operating system pages in whatever a batch touches. The dataset then stops being limited by RAM
and starts being limited by disk, which is the cheap resource.

COST. Uncompressed is roughly three times larger on disk -- the wide corpus goes from 6.7 GB to about
21 GB -- and training is somewhat slower the first epoch, because pages come from disk instead of the
page cache. Both are worth it against discarding two thirds of the corpus.

USAGE.
  convert_npz_to_mmap.py --in w2_train_mixed.npz --out w2_train_mixed_mmap/
  train_depth3.py --train w2_train_mixed_mmap/ ...      # a directory, not a file

The directory is also readable by the unchanged trainer: `Batches` accepts either form, so a converted
set can be compared against the original without a second code path at the call site.
"""
import argparse
import os
import sys

import numpy as np

# Kept in step with `Batches.ARRAYS` and `Batches.UCI_ARRAYS`; a name here that the trainer does not
# read is harmless, a name the trainer reads and this misses is not, so the second list is optional.
REQUIRED = ("X", "H", "bits", "nof_bits", "alloc_first_prb", "alloc_nof_prb", "dmrs_bitmap",
            "qm", "sigma2_over_r2", "snr_nominal", "tb_bits", "nof_tb_bits", "rnti", "n_id",
            "rv")
OPTIONAL = ("uci_nof_harq_ack", "uci_ack_re", "uci_ack_rvd_re", "uci_csi1_re")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="src", required=True, help="compressed .npz to read")
    ap.add_argument("--out", dest="dst", required=True, help="directory to write the .npy files into")
    ap.add_argument("--only", default="", help="comma-separated subset of array names, for a quick trial")
    args = ap.parse_args()

    os.makedirs(args.dst, exist_ok=True)
    wanted = [n.strip() for n in args.only.split(",") if n.strip()] if args.only else None

    total = 0
    with np.load(args.src, allow_pickle=True) as archive:
        present = set(archive.files)
        missing_required = [n for n in REQUIRED if n not in present]
        if missing_required:
            print(f"FATAL: archive is missing required array(s): {', '.join(missing_required)}",
                  file=sys.stderr)
            return 1
        names = [n for n in (REQUIRED + OPTIONAL) if n in present]
        has_uci = OPTIONAL[0] in present
        if not has_uci:
            # Recorded so the trainer can tell "this dataset predates control information" from
            # "this conversion dropped it", which are different things.
            print("note: no UCI arrays in the source; the trainer will use the empty layout")
        if wanted:
            names = [n for n in names if n in wanted]

        for name in names:
            array = archive[name]
            path = os.path.join(args.dst, name + ".npy")
            np.save(path, array)
            size = os.path.getsize(path)
            total += size
            print(f"  {name:20s} {str(array.shape):28s} {array.dtype}  {size / 1048576:8.1f} MB")

    print(f"wrote {len(names)} array(s), {total / 1048576:.0f} MB, into {args.dst}")
    print(f"train with: --train {args.dst}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
