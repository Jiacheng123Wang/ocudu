#!/usr/bin/env python3
"""Does the bundle's publication still carry the channel? The dynamic range it keeps, per modulation.

WHY IT EXISTS (design section 19.215). The scheduler reads the arm's own publication -- in both
reporting modes the SINR comes from the network's noise head, not from the classical estimator --
so a bundle whose publication is COMPRESSED hands the scheduler nearly the same number over very
different channels, and no affine calibration can put back information that is not there. Measured
on 2026-10-19: the deployed c64 keeps 0.96 and 1.15 of the classical chain's spread (64QAM and
256QAM, correlation 0.73 and 0.97), while a c32 trained with the base noise weight kept 0.45 and
0.24 with 256QAM correlation 0.05 -- its 256QAM publication was a near-constant 21 dB.

What is reported, per modulation, over the receptions BOTH arms computed (the arm's number and the
classical chain's for the same grid):

    sd ratio   standard deviation of the arm's publication over that of the classical chain's
    pearson r  how much of the classical chain's variation the arm reproduces
    arm spread the arm's own min..max, which is what the scheduler actually sees move

A bundle is usable for AMC when sd ratio is close to 1 and r is high; below about 0.8 it is handing
the scheduler a flattened picture, and a correlation near zero means the number is a constant.

Usage:
  reporting_range.py --label "new c32" --ab work_tmp/cmp/ab_attach_x.csv --ab work_tmp/cmp/ab_ai22_x.csv ...
  reporting_range.py --label ft --ab ... --json        (one JSON line per modulation)
"""

import argparse
import collections
import csv
import json
import math
import sys


def sd(values):
    mean = sum(values) / len(values)
    return mean, math.sqrt(sum((v - mean) ** 2 for v in values) / len(values))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ab", action="append", required=True, help="attach_ab.py CSV")
    parser.add_argument("--label", default="arm")
    parser.add_argument("--min-n", type=int, default=10)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    pairs = collections.defaultdict(list)
    for path in args.ab:
        for row in csv.DictReader(open(path)):
            # Only receptions the arm actually COMPUTED: a degraded one carries the classical
            # chain's number by construction, and counting it as "the arm's publication" would
            # report a perfect range for a head that never ran.
            if row.get("crc_ai") and row.get("arm_ran") == "True":
                pairs[row["mod"]].append((float(row["sinr_classic"]), float(row["sinr_ai"])))

    if not args.json:
        print(f"{args.label}: publication dynamic range against the classical chain "
              f"({sum(len(v) for v in pairs.values())} receptions the arm computed)")
        print(f"  {'mod':>6} {'n':>4} {'classical mean':>14} {'sd':>6} | {'arm mean':>8} {'sd':>6} "
              f"{'range':>12} | {'sd ratio':>8} {'r':>6}  verdict")

    worst = None
    for mod in ("QPSK", "16QAM", "64QAM", "256QAM"):
        rows = pairs.get(mod, [])
        if len(rows) < args.min_n:
            continue
        xs = [p[0] for p in rows]
        ys = [p[1] for p in rows]
        mx, sx = sd(xs)
        my, sy = sd(ys)
        r = (sum((x - mx) * (y - my) for x, y in rows) / (len(rows) * sx * sy)) if sx and sy else float("nan")
        ratio = sy / sx if sx else float("nan")
        verdict = ("carries the channel" if ratio >= 0.8 and r >= 0.6 else
                   "flattened" if ratio >= 0.4 else "near-constant")
        worst = min(worst, ratio) if worst is not None else ratio
        if args.json:
            print(json.dumps({"label": args.label, "mod": mod, "n": len(rows), "sd_ratio": round(ratio, 3),
                              "pearson_r": round(r, 3), "arm_range": [round(min(ys), 2), round(max(ys), 2)],
                              "verdict": verdict}))
        else:
            print(f"  {mod:>6} {len(rows):>4} {mx:>14.2f} {sx:>6.2f} | {my:>8.2f} {sy:>6.2f} "
                  f"{min(ys):>6.1f}-{max(ys):<5.1f} | {ratio:>8.2f} {r:>+6.2f}  {verdict}")
    if not args.json:
        print(f"  worst sd ratio {worst:.2f}" if worst is not None else "  no modulation had enough receptions")
    return 0


if __name__ == "__main__":
    sys.exit(main())
