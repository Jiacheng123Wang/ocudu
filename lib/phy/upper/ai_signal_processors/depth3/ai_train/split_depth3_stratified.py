#!/usr/bin/env python3
"""Split the depth-3 datasets into train/monitor/test by 3-D bin (modulation x width x SNR).

WHY A BINNED SPLIT RATHER THAN A RANDOM ONE. The corpus is not uniform: wide grants at high SNR arrive by the tens of
thousands while a narrow grant at two decibels arrives a handful of times. A random split puts almost every rare case in
one side and leaves the other with a bin it cannot measure, so a gate computed on it says nothing about the cells that
matter. Every split is therefore drawn PER BIN, and a bin too small to split goes entirely to training and is reported
as such instead of being quietly halved.

WIDTH BANDS come from the deployed envelope: 1-3 PRB is where attach and small traffic live, 4-8 is the narrow band the
first generation trained on, 9-24 is the middle the ping-1400 leg fills, and above 24 is iperf. SNR bands follow the
range the model is expected to run in. Both are arguments, not constants, because the bands should be a statement about
the data being built, not a hard-coded opinion.

Usage:
  split_depth3_stratified.py --datasets DIR --out DIR [--per-bin 2000] [--test 0.1] [--monitor 0.1] [--seed 20261027]
                             [--include REGEX] [--exclude REGEX]
Output: <out>/census.csv (bin, n, train, monitor, test), <out>/split.csv (file,row,bin,split), and a summary.
"""
import argparse
import collections
import csv
import glob
import os
import random
import re

import numpy as np

MODS = {2: "QPSK", 4: "16QAM", 6: "64QAM", 8: "256QAM"}
WIDTH_EDGES = [3, 8, 24]                      # bands: <=3, 4-8, 9-24, >24
SNR_EDGES = [0.0, 6.0, 10.0, 14.0, 20.0, 30.0]   # bands: <0, 0-6, 6-10, 10-14, 14-20, 20-30, >=30


def width_band(prb):
    for edge in WIDTH_EDGES:
        if prb <= edge:
            return f"<={edge}"
    return f">{WIDTH_EDGES[-1]}"


def snr_band(snr):
    previous = None
    for edge in SNR_EDGES:
        if snr < edge:
            return f"<{edge:g}" if previous is None else f"{previous:g}-{edge:g}"
        previous = edge
    return f">={SNR_EDGES[-1]:g}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--datasets", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--per-bin", type=int, default=2000,
                    help="most samples any one bin may contribute to the split (the rest stay unused)")
    ap.add_argument("--test", type=float, default=0.10)
    ap.add_argument("--monitor", type=float, default=0.10)
    ap.add_argument("--min-to-split", type=int, default=20,
                    help="a bin smaller than this goes entirely to training: splitting it would leave nothing measurable")
    ap.add_argument("--seed", type=int, default=20261027)
    ap.add_argument("--include", default="", help="only files whose name matches")
    ap.add_argument("--exclude", default="", help="skip files whose name matches (e.g. a held-out leg)")
    args = ap.parse_args()

    files = sorted(glob.glob(os.path.join(args.datasets, "*.npz")))
    if args.include:
        files = [f for f in files if re.search(args.include, os.path.basename(f))]
    if args.exclude:
        files = [f for f in files if not re.search(args.exclude, os.path.basename(f))]
    if not files:
        raise SystemExit("no datasets matched")

    rng = random.Random(args.seed)
    # Gather every sample's bin and keep a shuffled candidate list per bin, so the quota is drawn at random per bin.
    per_bin = collections.defaultdict(list)
    total = 0
    for path in files:
        d = np.load(path, allow_pickle=True)
        qm = d["qm"].astype(int)
        prb = d["alloc_nof_prb"].astype(int)
        snr = d["snr_nominal"].astype(float)
        for i in range(len(qm)):
            if not np.isfinite(snr[i]):
                continue
            key = (MODS.get(qm[i], "?"), width_band(prb[i]), snr_band(snr[i]))
            per_bin[key].append((os.path.basename(path), i))
            total += 1
    for key in per_bin:
        rng.shuffle(per_bin[key])

    census_rows, split_rows = [], []
    for key in sorted(per_bin):
        members = per_bin[key]
        n = len(members)
        keep = members[: args.per_bin] if args.per_bin > 0 else members
        if n < args.min_to_split:
            n_test = n_monitor = 0
        else:
            n_test = max(1, int(len(keep) * args.test))
            n_monitor = max(1, int(len(keep) * args.monitor))
        n_test = min(n_test, max(0, len(keep) - 1))
        n_monitor = min(n_monitor, max(0, len(keep) - n_test - 1))
        for idx, (fname, row) in enumerate(keep):
            split = "test" if idx < n_test else ("monitor" if idx < n_test + n_monitor else "train")
            split_rows.append((fname, row, "|".join(key), split))
        census_rows.append({"bin": "|".join(key), "n": n, "kept": len(keep),
                            "train": len(keep) - n_test - n_monitor, "monitor": n_monitor, "test": n_test})

    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "census.csv"), "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=["bin", "n", "kept", "train", "monitor", "test"])
        writer.writeheader()
        writer.writerows(census_rows)
    with open(os.path.join(args.out, "split.csv"), "w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["file", "row", "bin", "split"])
        writer.writerows(split_rows)

    by_split = collections.Counter(r[3] for r in split_rows)
    small = [r for r in census_rows if r["n"] < args.min_to_split]
    print(f"files: {len(files)}  samples: {total}  bins: {len(census_rows)}")
    print(f"split sizes: train {by_split['train']}  monitor {by_split['monitor']}  test {by_split['test']}")
    print(f"bins too small to split (<{args.min_to_split}): {len(small)}  -> they are training-only")
    for r in sorted(small, key=lambda r: r["n"])[:12]:
        print(f"    {r['bin']:32s} n={r['n']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
