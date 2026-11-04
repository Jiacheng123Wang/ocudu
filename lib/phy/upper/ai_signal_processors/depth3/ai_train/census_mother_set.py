#!/usr/bin/env python3
"""Census a MOTHER SET into the same bins census_bins.py uses: modulation x width x SNR x population.

WHY THE MOTHER SET AND NOT THE CAPTURES. The draw works on decodable samples, so the bins that decide
whether a draw can be uniform per bin are the mother set's bins, not the captures'. census_bins.py
answers the other question -- what the corpus covers, undecodable samples included -- and both are
worth having, but only this one can be compared against the quotas a draw applies.

THE EDGES ARE IMPORTED FROM census_bins.py, so the census, the selector and the stratified splitter
cannot drift apart; a bin label is computed one way in this tree.

WHAT IT PRINTS. Totals per population and per modulation, the full bin table as self/harq pairs, the
empty bins (they are the coverage gaps), and what a uniform draw at a set of caps would yield -- that
last table is what a collection or a draw decision is actually made from.

Usage:
  census_mother_set.py [--dir DIR] [--csv OUT] [--caps 2500:250,8000:800]

--dir defaults to ~/ai_depth3/datasets_full_s2; --csv writes modulation,width,snr,population,samples.
"""
import argparse
import collections
import csv
import glob
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
from census_bins import WIDTH_EDGES, SNR_EDGES, width_band, snr_band   # noqa: E402

MODS = ("QPSK", "16QAM", "64QAM", "256QAM")
WIDTHS = tuple(f"<={e}" for e in WIDTH_EDGES) + (f">{WIDTH_EDGES[-1]}",)
# THE LABELS COME FROM snr_band ITSELF, one representative value per band: the labels written out by
# hand ("<0.0", "6.0", ...) did not match what the function returns ("<0", "0-6", ...), so every lookup
# missed and the table printed zeros under right-looking totals. Sampling the midpoint between
# consecutive edges is what makes the derivation impossible to get wrong: the first pass at this fix
# divided the edge by two instead, which labelled three bands "0-6" and made 256 bins out of 224.
def _band_labels():
    labels, previous = [], None
    for edge in SNR_EDGES:
        labels.append(snr_band(edge - 0.5 if previous is None else (previous + edge) / 2.0))
        previous = edge
    labels.append(snr_band(SNR_EDGES[-1] + 0.5))
    return tuple(labels)


SNRS = _band_labels()
assert len(SNRS) == len(SNR_EDGES) + 1 and len(set(SNRS)) == len(SNRS), SNRS


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=os.path.expanduser("~/ai_depth3/datasets_full_s2"))
    ap.add_argument("--csv", default="")
    ap.add_argument("--caps", default="1000:100,2500:250,5000:500,8000:800",
                    help="uniform-draw projections to print, as self:harq pairs")
    args = ap.parse_args()

    files = sorted(glob.glob(os.path.join(args.dir, "*.npz")))
    if not files:
        raise SystemExit(f"no .npz in {args.dir}")

    counts = collections.Counter()
    per_mod = collections.Counter()
    per_pop = collections.Counter()
    nan_snr = collections.Counter()

    for i, path in enumerate(files, 1):
        name = os.path.basename(path)
        pop = "harq" if name.endswith("_harq.npz") else "self"
        mod = next((m for m in MODS if f"_{m}_" in name), None)
        if mod is None:
            print(f"SKIP {name}: no known modulation in the name", file=sys.stderr)
            continue
        data = np.load(path)
        prb = np.asarray(data["alloc_nof_prb"])
        snr = np.asarray(data["snr_nominal"], dtype=float)
        for p, s in zip(prb, snr):
            per_mod[mod] += 1
            per_pop[pop] += 1
            if not np.isfinite(s):
                nan_snr[(mod, pop)] += 1
                continue
            counts[(mod, width_band(int(p)), snr_band(float(s)), pop)] += 1
        if i % 40 == 0:
            print(f"  {i}/{len(files)} files, {sum(per_pop.values()):,} samples", file=sys.stderr)

    total = sum(per_pop.values())
    if args.csv:
        with open(args.csv, "w", newline="") as fh:
            out = csv.writer(fh)
            out.writerow(["modulation", "width", "snr", "population", "samples"])
            for mod in MODS:
                for width in WIDTHS:
                    for snr in SNRS:
                        for pop in ("self", "harq"):
                            out.writerow([mod, width, snr, pop, counts.get((mod, width, snr, pop), 0)])
        print(f"wrote {args.csv}")

    print(f"\nfiles {len(files)}   samples {total:,}")
    print("population: " + "  ".join(f"{p} {per_pop[p]:,}" for p in ("self", "harq")))
    print("modulation: " + "  ".join(f"{m} {per_mod[m]:,} ({100 * per_mod[m] / total:.1f}%)" for m in MODS))
    if nan_snr:
        print("non-finite SNR: " + "  ".join(f"{k[0]}/{k[1]} {v}" for k, v in sorted(nan_snr.items())))

    print("\nbin table (self/harq)")
    print(f"{'mod':6s} {'width':5s} " + " ".join(f"{s:>15s}" for s in SNRS))
    for mod in MODS:
        for width in WIDTHS:
            cells = []
            for snr in SNRS:
                s_ = counts.get((mod, width, snr, "self"), 0)
                h_ = counts.get((mod, width, snr, "harq"), 0)
                cells.append(f"{s_:>7,d}/{h_:<7,d}")
            print(f"{mod:6s} {width:5s} " + " ".join(cells))

    nonempty = sum(1 for v in counts.values() if v)
    print(f"\nnon-empty bins: {nonempty} of {len(MODS) * len(WIDTHS) * len(SNRS) * 2}")
    empty = [k for k, v in sorted(counts.items()) if v == 0]
    if empty:
        print("empty bins: " + "; ".join(f"{k[0]} {k[1]} {k[2]} {k[3]}" for k in empty))

    print("\nuniform per-bin draw at each cap (total = sum over bins of min(count, cap))")
    for spec in args.caps.split(","):
        cs, _, ch = spec.partition(":")
        cs, ch = int(cs), int(ch or cs)
        take = {k: min(v, cs if k[3] == "self" else ch) for k, v in counts.items()}
        tot = sum(take.values())
        by_mod = collections.Counter()
        for k, v in take.items():
            by_mod[k[0]] += v
        mix = " / ".join(f"{100 * by_mod[m] / tot:.0f}%" for m in MODS)
        print(f"  cap {cs:,}/{ch:,}: {tot:9,} samples   QPSK/16QAM/64QAM/256QAM = {mix}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
