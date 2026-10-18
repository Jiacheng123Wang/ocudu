#!/usr/bin/env python3
"""Per-cell verdicts: the table that decides whether a bundle may be adopted.

WHY IT EXISTS (design sections 19.205/19.206). A single decode rate hides everything that
matters: the arm beat the classical chain by 13 points on one capture and lost 60 on the next,
because the two captures occupy different (modulation x width x code rate) cells. D2 was
"100/100" while the field failed, and both statements were true. So a bundle is judged HERE, in
cells, on recordings whose field verdicts are known.

Input: one or more CSV files written by `attach_ab.py` (`--out`), i.e. per reception:
modulation, rv, new_data, allocation width, code rate, and the two arms' verdicts and SINRs.
Only `new_data=1` receptions enter the verdict table: a retransmission replayed one-shot meets
an empty soft buffer, so its verdict is not the field's (the SINR column is still comparable).

Reported per cell: n, classical decode, arm decode, the difference, and -- for the goal's
acceptance -- whether the cell is a REGRESSION (arm worse than classical by more than --slack).

Usage:
  cell_report.py --ab work_tmp/cmp/ab_attach_ft.csv --ab work_tmp/cmp/ab_ai22_ft.csv \
      --label ft [--slack 2] [--fail-on-regression]
"""

import argparse
import collections
import csv
import sys


def width_band(nof_prb):
    n = int(nof_prb)
    return "<=4" if n <= 4 else ("5-8" if n <= 8 else ("9-24" if n <= 24 else "25+"))


def rate_band(rate):
    r = float(rate)
    return "<0.70" if r < 0.70 else ("0.70-0.85" if r < 0.85 else ("0.85-0.92" if r < 0.92 else ">=0.92"))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ab", action="append", required=True, help="attach_ab.py CSV")
    parser.add_argument("--label", default="arm")
    parser.add_argument("--slack", type=int, default=2,
                        help="receptions a cell may lose before it is called a regression")
    parser.add_argument("--min-n", type=int, default=5, help="cells smaller than this are pooled, not judged")
    parser.add_argument("--fail-on-regression", action="store_true")
    args = parser.parse_args()

    rows = []
    for path in args.ab:
        for row in csv.DictReader(open(path)):
            if row.get("crc_ai") and row.get("new_data") == "1":
                row["source"] = path.rsplit("/", 1)[-1]
                rows.append(row)
    if not rows:
        raise SystemExit("no first-transmission rows with both arms")

    cells = collections.defaultdict(lambda: [0, 0, 0])
    for row in rows:
        key = (row["mod"], width_band(row["nof_prb"]), rate_band(row["code_rate"]))
        cell = cells[key]
        cell[0] += 1
        cell[1] += row["crc_classic"] == "OK"
        cell[2] += row["crc_ai"] == "OK"

    total = len(rows)
    classical = sum(1 for row in rows if row["crc_classic"] == "OK")
    arm = sum(1 for row in rows if row["crc_ai"] == "OK")
    print(f"=== {args.label}: {total} first transmissions from {len(args.ab)} capture set(s)")
    print(f"    classical {classical} ({100.0 * classical / total:.1f}%)   "
          f"arm {arm} ({100.0 * arm / total:.1f}%)   difference {arm - classical:+d}")
    print()
    print(f"    {'mod':>7} {'width':>6} {'rate':>10} {'n':>5} {'classical':>10} {'arm':>7} {'diff':>6}  verdict")
    regressions = []
    small = [0, 0, 0]
    for key in sorted(cells, key=lambda k: (-cells[k][0], k)):
        n, classic, ai = cells[key]
        verdict = ""
        if n < args.min_n:
            small[0] += n
            small[1] += classic
            small[2] += ai
            verdict = "pooled (n < %d)" % args.min_n
        elif ai < classic - args.slack:
            verdict = "REGRESSION"
            regressions.append((key, n, classic, ai))
        elif ai > classic + args.slack:
            verdict = "gain"
        print(f"    {key[0]:>7} {key[1]:>6} {key[2]:>10} {n:>5} {100.0 * classic / n:>9.1f}% "
              f"{100.0 * ai / n:>6.1f}% {ai - classic:>+6d}  {verdict}")
    if small[0]:
        print(f"    {'(pooled)':>7} {'':>6} {'':>10} {small[0]:>5} {100.0 * small[1] / small[0]:>9.1f}% "
              f"{100.0 * small[2] / small[0]:>6.1f}% {small[2] - small[1]:>+6d}  n < {args.min_n}")

    print()
    if regressions:
        print(f"REGRESSIONS ({len(regressions)} cell(s), arm worse than classical by more than {args.slack}):")
        for key, n, classic, ai in regressions:
            print(f"  {key[0]} {key[1]} PRB rate {key[2]}: n={n} classical {classic} arm {ai} ({ai - classic:+d})")
        if args.fail_on_regression:
            return 1
    else:
        print(f"no cell regresses by more than {args.slack} reception(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
