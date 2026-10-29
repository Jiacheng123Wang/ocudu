#!/usr/bin/env python3
"""Inventory the BUILT datasets per bin: how many samples are self (crc=OK, rv=0) and how many were recovered by HARQ.

WHY IT IS SEPARATE FROM THE CENSUS. census_bins.py counts what was CAPTURED. This counts what survives into the datasets, which is the number
that matters when deciding whether a bin can train: a capture that never decoded contributes nothing, and a sample recovered from a later
retransmission is worth more than its count suggests because it is the rare kind.

The bins are the ones select_by_bin.py, split_depth3_stratified.py and census_bins.py use, so all four agree by construction. The per-sample
SNR here is the dataset's own `snr_nominal` (the value the builder was given), not a re-estimate from the sidecar.

Usage: inventory_datasets.py [--dir datasets_bin] [--csv OUT] [--md OUT]
"""
import argparse
import collections
import glob
import os

import numpy as np

MODULATIONS = ["QPSK", "16QAM", "64QAM", "256QAM"]
WIDTHS = ["<=3", "<=8", "<=24", ">24"]
SNRS = ["<0", "0-6", "6-10", "10-14", "14-20", "20-30", ">=30"]


def width_band(prb):
    for edge in (3, 8, 24):
        if prb <= edge:
            return "<=3" if edge == 3 else ("<=8" if edge == 8 else "<=24")
    return ">24"


def snr_band(snr):
    for lo, hi, name in ((-1e9, 0, "<0"), (0, 6, "0-6"), (6, 10, "6-10"), (10, 14, "10-14"),
                         (14, 20, "14-20"), (20, 30, "20-30")):
        if lo <= snr < hi:
            return name
    return ">=30"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="datasets_bin")
    ap.add_argument("--csv", default="")
    ap.add_argument("--md", default="")
    args = ap.parse_args()

    files = sorted(glob.glob(os.path.join(args.dir, "*.npz")))
    if not files:
        print(f"nothing to inventory in {args.dir}")
        return 2

    # counts[(modulation, width, snr, population)] = n
    counts = collections.Counter()
    unknown_mod = 0
    for path in files:
        name = os.path.basename(path)
        modulation = next((m for m in MODULATIONS if m in name), None)
        if modulation is None:
            unknown_mod += 1
            continue
        population = "harq" if name.endswith("_harq.npz") else "self"
        data = np.load(path, allow_pickle=True)
        prb = np.asarray(data["alloc_nof_prb"])
        snr = np.asarray(data["snr_nominal"], dtype=float)
        for p, s in zip(prb, snr):
            counts[(modulation, width_band(int(p)), snr_band(float(s)), population)] += 1

    total_self = sum(v for k, v in counts.items() if k[3] == "self")
    total_harq = sum(v for k, v in counts.items() if k[3] == "harq")

    lines = ["| 调制 | 宽度 | " + " | ".join(SNRS) + " |",
             "|---|---|" + "---|" * len(SNRS)]
    for m in MODULATIONS:
        for w in WIDTHS:
            cells = []
            for s in SNRS:
                a = counts.get((m, w, s, "self"), 0)
                b = counts.get((m, w, s, "harq"), 0)
                cells.append(f"{a:,} / {b:,}" if (a or b) else "—")
            lines.append(f"| **{m}** | `{w}` | " + " | ".join(cells) + " |")
    table = "\n".join(lines)

    print(f"{len(files)} file(s)")
    print(f"self samples: {total_self:,}   harq samples: {total_harq:,}   total: {total_self + total_harq:,}")
    if unknown_mod:
        print(f"files with no recognisable modulation in the name: {unknown_mod}")
    print()
    print(table)

    print("\n--- per modulation ---")
    for m in MODULATIONS:
        a = sum(v for k, v in counts.items() if k[0] == m and k[3] == "self")
        b = sum(v for k, v in counts.items() if k[0] == m and k[3] == "harq")
        print(f"  {m:6s} self {a:6,}  harq {b:6,}")
    print("\n--- bins with nothing ---")
    empty = [(m, w, s) for m in MODULATIONS for w in WIDTHS for s in SNRS
             if counts.get((m, w, s, "self"), 0) + counts.get((m, w, s, "harq"), 0) == 0]
    print(f"  {len(empty)} of {len(MODULATIONS) * len(WIDTHS) * len(SNRS)} bins are empty")
    for e in empty:
        print("   ", e)

    if args.csv:
        with open(args.csv, "w") as handle:
            handle.write("modulation,width,snr,self,harq\n")
            for m in MODULATIONS:
                for w in WIDTHS:
                    for s in SNRS:
                        handle.write(f"{m},{w},{s},{counts.get((m, w, s, 'self'), 0)},{counts.get((m, w, s, 'harq'), 0)}\n")
        print(f"\nwrote {args.csv}")
    if args.md:
        with open(args.md, "w") as handle:
            handle.write(table + "\n")
        print(f"wrote {args.md}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
