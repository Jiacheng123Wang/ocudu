#!/usr/bin/env python3
"""Independent check of the s2 MCS00 batch: capture accounting and bin totals.

Written from scratch (does not import bin_s2.py). It re-reads the capture metadata and the dump
index.csv files, re-derives every bin, and prints the per-leg accounting plus the table so the
numbers can be compared cell by cell with bin_s2.py's output.
"""
import collections
import glob
import os
import sys

LEGS = [f"s2_mcs00_ping_1031{c}" for c in "abcdefgh"]
HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = "/Volumes/T7_Corpus/corpus"

WIDTHS = ["<=3", "<=8", "<=24", ">24"]
SNRS = ["<0", "0-6", "6-10", "10-14", "14-20", "20-30", ">=30"]


def wbin(n):
    if n <= 3:
        return "<=3"
    if n <= 8:
        return "<=8"
    if n <= 24:
        return "<=24"
    return ">24"


def sbin(x):
    for edge, name in ((0, "<0"), (6, "0-6"), (10, "6-10"), (14, "10-14"), (20, "14-20"), (30, "20-30")):
        if x < edge:
            return name
    return ">=30"


counts = collections.Counter()
tot_cap = tot_idx = tot_ok = tot_bin = tot_tb = 0
per = []
for leg in LEGS:
    cdir = os.path.join(CORPUS, leg)
    caps = sorted(b for b in os.listdir(cdir)
                  if b.endswith(".txt") and not b.startswith("._") and not b.endswith("_ce.txt"))
    prb = {}
    for b in caps:
        try:
            with open(os.path.join(cdir, b), errors="ignore") as f:
                for line in f:
                    if line.startswith("alloc_nof_rb="):
                        prb[b[:-4]] = int(line.split("=", 1)[1])
                        break
        except OSError:
            continue
    idx = os.path.join(HERE, leg, "index.csv")
    if not os.path.exists(idx):
        print(f"MISSING index.csv for {leg}", file=sys.stderr)
        per.append((leg, len(caps), None, None, None, None))
        continue
    nrows = nok = nbin = 0
    with open(idx) as f:
        next(f, None)
        for line in f:
            p = line.rstrip("\n").split(",")
            nrows += 1
            if len(p) < 5 or p[2] != "OK":
                continue
            nok += 1
            n = prb.get(p[0])
            try:
                s = float(p[3])
            except ValueError:
                continue
            if n is None:
                continue
            counts[(wbin(n), sbin(s))] += 1
            nbin += 1
    ntb = len(glob.glob(os.path.join(HERE, leg, "tb", "*.bits")))
    per.append((leg, len(caps), nrows, nok, nbin, ntb))
    tot_cap += len(caps)
    tot_idx += nrows
    tot_ok += nok
    tot_bin += nbin
    tot_tb += ntb

print("per-leg: leg captures index_rows crc_ok binned tb_files")
for leg, ncap, nrows, nok, nbin, ntb in per:
    print(f"  {leg}  {ncap:,}  {nrows if nrows is not None else 'MISSING'}  "
          f"{nok if nok is not None else '-'}  {nbin if nbin is not None else '-'}  {ntb}")

print()
print(f"TOTAL captures={tot_cap:,} index_rows={tot_idx:,} crc_ok={tot_ok:,} binned={tot_bin:,} tb_files={tot_tb:,}")
print()
print("| MCS | PRB 宽度 | " + " | ".join(SNRS) + " | sum |")
print("|---|---|" + "|".join(["---"] * len(SNRS)) + "|---|")
table_sum = 0
for w in WIDTHS:
    row = [counts.get((w, s), 0) for s in SNRS]
    table_sum += sum(row)
    cells = [f"{v:,}" if v else "—" for v in row]
    print(f"| **MCS00** | `{w}` | " + " | ".join(cells) + f" | {sum(row):,} |")
print()
print(f"table cell sum = {table_sum:,}   crc_ok sum = {tot_ok:,}   binned sum = {tot_bin:,}", file=sys.stderr)
print("col sums: " + ", ".join(f"{s}={sum(counts.get((w, s), 0) for w in WIDTHS):,}" for s in SNRS),
      file=sys.stderr)
