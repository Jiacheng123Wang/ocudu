#!/usr/bin/env python3
"""Bin an s2 batch by MCS x PRB width x SNR, counting only the receptions that decoded.

Data sources, and why both are needed:
  - the capture directory (cap_*.txt) supplies alloc_nof_rb, the grant for that slot. The capture metadata does NOT
    carry a measured SINR.
  - the dump's index.csv supplies sinr_db, which only exists because the replay measured it. So a leg must be dumped
    before it can be binned, and a leg that is not dumped yet is skipped with a warning rather than silently counted as
    empty.
  - the MCS comes from the leg name (s2_mcsXX_...).

Only rows with crc=OK are counted. The table in the corpus document describes samples, and a sample is a reception that
decoded; counting the failed ones that happen to carry an SINR would inflate every cell by about five percent and make the
numbers incomparable with the built-dataset table.

Usage:
  bin_s2.py [--dump DIR] [--corpus DIR] [--legs name ...]
Without --legs it bins every leg that has a dump directory.
"""
import argparse
import collections
import glob
import os
import re
import sys

WIDTHS = ["<=3", "<=8", "<=24", ">24"]
SNRS = ["<0", "0-6", "6-10", "10-14", "14-20", "20-30", ">=30"]


def wbin(n):
    return "<=3" if n <= 3 else "<=8" if n <= 8 else "<=24" if n <= 24 else ">24"


def sbin(x):
    if x < 0:
        return "<0"
    if x < 6:
        return "0-6"
    if x < 10:
        return "6-10"
    if x < 14:
        return "10-14"
    if x < 20:
        return "14-20"
    if x < 30:
        return "20-30"
    return ">=30"


def leg_mcs(leg):
    m = re.search(r"mcs(\d+)", leg)
    return f"MCS{m.group(1)}" if m else "MCS??"


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", default=os.path.join(os.path.dirname(os.path.abspath(__file__))))
    ap.add_argument("--corpus", default="/Volumes/T7_Corpus/corpus")
    ap.add_argument("--legs", nargs="*", default=[])
    args = ap.parse_args(argv)

    # DUMP DIRECTORIES CARRY A tb_ PREFIX, the capture directories do not. The two are otherwise named identically --
    # tb_s2_mcs00_ping_1031a holds the replay results of the captures in s2_mcs00_ping_1031a -- which matches how the w2
    # batch was named (tb_w2_harq_<leg> beside w2_harq_<leg>) and keeps a dump from being mistaken for captures.
    legs = args.legs or sorted(
        os.path.basename(d.rstrip("/"))[len("tb_"):]
        for d in glob.glob(os.path.join(args.dump, "tb_s2_*")) if os.path.isdir(d))

    counts = collections.Counter()          # (mcs, width, snr) -> n
    per_leg = []
    total_binned = 0
    for leg in legs:
        idx = os.path.join(args.dump, "tb_" + leg, "index.csv")
        if not os.path.exists(idx):
            print(f"  [skip] {leg}: no index.csv (not dumped yet)", file=sys.stderr)
            continue
        prb = {}
        for p in glob.glob(os.path.join(args.corpus, leg, "*.txt")):
            b = os.path.basename(p)
            if b.startswith("._") or b.endswith("_ce.txt"):
                continue
            unit = b[:-4]
            try:
                with open(p, "r", errors="ignore") as f:
                    for line in f:
                        if line.startswith("alloc_nof_rb="):
                            prb[unit] = int(line.split("=")[1])
                            break
            except OSError:
                continue
        mcs = leg_mcs(leg)
        n_ok = n_use = 0
        with open(idx) as f:
            next(f, None)
            for line in f:
                parts = line.rstrip("\n").split(",")
                if len(parts) < 5 or parts[2] != "OK":
                    continue
                n_ok += 1
                try:
                    s = float(parts[3])
                except ValueError:
                    continue
                n = prb.get(parts[0])
                if n is None:
                    continue
                counts[(mcs, wbin(n), sbin(s))] += 1
                n_use += 1
        total_binned += n_use
        per_leg.append((leg, mcs, len(prb), n_ok, n_use))
        print(f"  {leg}: PRB 索引 {len(prb)} 条，crc=OK {n_ok}，已分箱 {n_use}", file=sys.stderr)

    mcses = sorted({k[0] for k in counts})
    print()
    print("| MCS | PRB 宽度 | " + " | ".join(SNRS) + " |")
    print("|---|---|" + "|".join(["---"] * len(SNRS)) + "|")
    for m in mcses:
        for w in WIDTHS:
            row = [counts.get((m, w, s), 0) for s in SNRS]
            if sum(row) == 0:
                continue
            cells = [f"{v:,}" if v else "—" for v in row]
            print(f"| **{m}** | `{w}` | " + " | ".join(cells) + " |")

    col = {s: sum(counts.get((m, w, s), 0) for m in mcses for w in WIDTHS) for s in SNRS}
    print()
    print("合计: {:,} 条；按 SNR 列: {}".format(
        total_binned, ", ".join(f"{s}={col[s]:,}" for s in SNRS)), file=sys.stderr)
    print("\n逐腿:", file=sys.stderr)
    for leg, mcs, n_prb, n_ok, n_use in per_leg:
        print(f"  {leg:26s} {mcs}  cap={n_prb:,}  crcOK={n_ok:,}  binned={n_use:,}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
