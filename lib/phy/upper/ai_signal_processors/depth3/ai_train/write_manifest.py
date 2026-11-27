#!/usr/bin/env python3
"""Write the per-leg manifest of a mother-set build: <out>/_manifest_<leg>.csv.

WHAT IT ANSWERS. For every (modulation, population) of one leg: how many samples were built, how
many index rows that population had, and how many were rejected. The two rejection causes are not
separated here on purpose -- the build log prints the DM-RS/pilot rejections per pair, and the
unknown-iteration drops are the rows whose `iterations` is empty -- but the totals have to add up,
which is what makes a short build visible.

WHY IT NEEDS THE CAPTURES. `index.csv` does not carry the modulation; it is in each capture's PDU
sidecar (`<unit>.txt`, line `modulation=`), and that is also how `build_mother_set.sh` decides which
datasets exist. So the manifest is written while the captures are still on disk, before the watcher
reclaims them.

Usage: write_manifest.py --out DIR --dump DIR --corpus DIR --leg LEG
       write_manifest.py --out DIR --dump DIR --corpus DIR     (leg derived from the dump name)
"""
import argparse
import collections
import csv
import glob
import os
import sys
from concurrent.futures import ThreadPoolExecutor

import numpy as np

MODS = ("QPSK", "16QAM", "64QAM", "256QAM")


def modulation_of(path):
    """The modulation recorded in one PDU sidecar, or None if it cannot be read."""
    try:
        with open(path) as handle:
            for line in handle:
                if line.startswith("modulation="):
                    return line.strip().split("=", 1)[1]
    except OSError:
        return None
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True, help="the mother-set directory the npz were written to")
    ap.add_argument("--dump", required=True, help="the dump directory (tb_<leg>)")
    ap.add_argument("--corpus", required=True, help="the corpus root holding <leg>/")
    ap.add_argument("--leg", default="", help="leg name (default: the dump directory's name without tb_)")
    ap.add_argument("--jobs", type=int, default=8, help="sidecars read in parallel")
    args = ap.parse_args()

    leg = args.leg or os.path.basename(os.path.normpath(args.dump))
    if leg.startswith("tb_"):
        leg = leg[3:]
    cap = os.path.join(args.corpus, leg)
    if not os.path.isdir(cap):
        sys.exit(f"no capture directory {cap}: the modulation of each index row cannot be resolved, "
                 f"so no manifest can be written")

    built = {}
    for path in sorted(glob.glob(os.path.join(args.out, leg + "_*.npz"))):
        name = os.path.basename(path)
        mod = next((m for m in MODS if m in name), None)
        if mod is None:
            continue
        pop = "harq" if name.endswith("_harq.npz") else "self"
        with np.load(path, allow_pickle=True) as data:
            built[(mod, pop)] = len(data["X"])

    rows = []
    unresolved = 0
    for pop, index_name in (("self", "index_self.csv"), ("harq", "index_harq_only.csv")):
        path = os.path.join(args.dump, index_name)
        if not os.path.exists(path):
            continue
        with open(path) as handle:
            units = [r["unit"] for r in csv.DictReader(handle)]
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            mods = list(pool.map(lambda u: modulation_of(os.path.join(cap, u + ".txt")), units))
        per = collections.Counter(m for m in mods if m in MODS)
        unresolved += sum(1 for m in mods if m not in MODS)
        for mod, count in per.items():
            rows.append((leg, mod, pop, built.get((mod, pop), 0), count, count - built.get((mod, pop), 0)))

    rows.sort(key=lambda r: (MODS.index(r[1]), r[2]))
    path = os.path.join(args.out, f"_manifest_{leg}.csv")
    with open(path, "w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["leg", "modulation", "population", "samples", "index_rows", "rejected_total"])
        writer.writerows(rows)

    print(f"{'mod':7s} {'pop':5s} {'index':>7s} {'built':>7s} {'rejected':>8s}")
    for _, mod, pop, samples, index_rows, rejected in rows:
        print(f"{mod:7s} {pop:5s} {index_rows:7d} {samples:7d} {rejected:8d}")
    print(f"total built {sum(r[3] for r in rows)} of {sum(r[4] for r in rows)} index rows"
          + (f" ({unresolved} sidecar(s) unreadable)" if unresolved else ""))
    print(f"wrote {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
