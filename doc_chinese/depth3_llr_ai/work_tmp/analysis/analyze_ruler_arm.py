#!/usr/bin/env python3
"""Score one or two AI arms on the ruler against the CPU chain, with paired statistics.

WHY THIS EXISTS.  The per-unit CSVs written by the ruler replays (`ruler_*.csv`, one row per unit)
and the ruler's own CPU reference (`ruler_s2_amc_5000_index.csv`, whose `tb_bits` is already the
DELIVERED bits: 0 for `crc=no`) are enough to answer every question the runbook asks -- delivered
bits, the paired bootstrap interval, McNemar on the decode decisions, and the modulation/width
breakdown -- but the arithmetic was being retyped for every measurement, which is how a baseline
gets mixed up.  One script, one baseline, no hand arithmetic.

THE BASELINE IS THE INDEX FILE, NOT A CONSTANT.  `tb_bits` there is the CPU chain's DELIVERED bits
(4 105 OK / 11 565 432 on the 5 000-unit ruler), so the comparison is paired per unit -- which is
what the bootstrap and McNemar need.  A constant baseline cannot give either.

Usage:
  analyze_ruler_arm.py --arm NAME=CSV [--arm NAME=CSV] --cpu-index INDEX.csv --corpus CORPUS_DIR

Each `--arm` CSV is either the `measure_arm.py` shape (`unit,status,bits`) or the
`run_arm_ext.py` shape (`unit,...,status,crc,tbs_bytes,tbs_bits,...`).  Only `status == OK`
contributes bits, per the runbook's metric definition.
"""

import argparse
import csv
import math
import os
import random
import sys
from collections import defaultdict

BOOTSTRAP_REPS = 2000
BOOTSTRAP_SEED = 20261102
WIDTH_BUCKETS = (("<=3", 0, 3), ("<=8", 4, 8), ("<=24", 9, 24), (">24", 25, 10**9))


def read_arm(path):
    """unit -> delivered bits, and the OK set.  Delivered bits only for `status == OK`."""
    bits, ok = {}, set()
    with open(path, newline="") as handle:
        reader = csv.DictReader(handle)
        fields = reader.fieldnames or []
        bits_key = "bits" if "bits" in fields else "tbs_bits"
        for row in reader:
            unit = row["unit"]
            won = row.get("status") == "OK"
            bits[unit] = int(row[bits_key] or 0) if won else 0
            if won:
                ok.add(unit)
    return bits, ok


def read_cpu_index(path):
    """unit -> delivered bits (index `tb_bits` is 0 on KO), plus the OK set."""
    bits, ok = {}, set()
    with open(path, newline="") as handle:
        for row in csv.DictReader(handle):
            unit = row["unit"]
            won = row.get("crc") == "OK"
            bits[unit] = int(row.get("tb_bits") or 0) if won else 0
            if won:
                ok.add(unit)
    return bits, ok


def read_sidecar_geometry(corpus, units):
    """unit -> (modulation, alloc_nof_rb); the breakdown needs both (runbook section 8.3)."""
    geometry = {}
    for unit in units:
        mod, rb = "?", 0
        try:
            with open(os.path.join(corpus, unit + ".txt")) as handle:
                for line in handle:
                    if line.startswith("modulation="):
                        mod = line.split("=", 1)[1].strip()
                    elif line.startswith("alloc_nof_rb="):
                        rb = int(line.split("=", 1)[1])
        except OSError:
            pass
        geometry[unit] = (mod, rb)
    return geometry


def bootstrap_ci(diffs):
    """Paired bootstrap over UNITS: every rep draws 5 000 units WITH replacement."""
    rng = random.Random(BOOTSTRAP_SEED)
    n = len(diffs)
    reps = sorted(sum(diffs[rng.randrange(n)] for _ in range(n)) for _ in range(BOOTSTRAP_REPS))
    return reps[BOOTSTRAP_REPS // 40], reps[-BOOTSTRAP_REPS // 40 - 1]


def mcnemar(a_ok, b_ok, universal):
    """Two-sided McNemar with continuity correction; only discordant units count."""
    x = len(a_ok - b_ok)
    y = len(b_ok - a_ok)
    if x + y == 0:
        return x, y, 1.0
    z = (abs(x - y) - 1.0) / math.sqrt(x + y)
    return x, y, math.erfc(abs(z) / math.sqrt(2.0))


def summarise(name, bits, ok, universal, cpu_bits, cpu_ok):
    total = sum(bits[u] for u in universal)
    cpu = sum(cpu_bits[u] for u in universal)
    diffs = [bits[u] - cpu_bits[u] for u in universal]
    lo, hi = bootstrap_ci(diffs)
    x, y, p = mcnemar(ok, cpu_ok, universal)
    print(f"== {name} ==")
    print(f"  units {len(universal)}  OK {len(ok)} ({100.0*len(ok)/len(universal):.2f}%)  bits {total:,}")
    print(f"  CPU   OK {len(cpu_ok)}  bits {cpu:,}")
    print(f"  vs CPU {total-cpu:+,} ({100.0*(total-cpu)/cpu:+.2f}%)  "
          f"bootstrap 95% CI [{lo:+,}, {hi:+,}]  ({100.0*lo/cpu:+.2f}% .. {100.0*hi/cpu:+.2f}%)")
    print(f"  McNemar arm-only {x} : cpu-only {y}   p = {p:.3g}")
    return total, len(ok)


def breakdown(bits, cpu_bits, ok, cpu_ok, geometry, universal):
    def table(keyfn, title, order):
        groups = defaultdict(lambda: [0, 0, 0, 0, 0])  # arm bits, cpu bits, arm ok, cpu ok, n
        for unit in universal:
            key = keyfn(geometry[unit])
            row = groups[key]
            row[0] += bits[unit]
            row[1] += cpu_bits[unit]
            row[2] += 1 if unit in ok else 0
            row[3] += 1 if unit in cpu_ok else 0
            row[4] += 1
        print(f"  -- by {title} --")
        print(f"     {'group':10s} {'units':>6s} {'arm bits':>12s} {'cpu bits':>12s} {'delta':>11s} "
              f"{'arm OK':>7s} {'cpu OK':>7s} {'arm-cpu OK':>11s}")
        for key in order:
            if key not in groups:
                continue
            arm_bits, cpu_bits_g, arm_ok, cpu_ok_g, n = groups[key]
            print(f"     {key:10s} {n:6d} {arm_bits:12,d} {cpu_bits_g:12,d} {arm_bits-cpu_bits_g:+11,d} "
                  f"{arm_ok:7d} {cpu_ok_g:7d} {arm_ok-cpu_ok_g:+11d}")
        extra = sorted(k for k in groups if k not in order)
        for key in extra:
            arm_bits, cpu_bits_g, arm_ok, cpu_ok_g, n = groups[key]
            print(f"     {key:10s} {n:6d} {arm_bits:12,d} {cpu_bits_g:12,d} {arm_bits-cpu_bits_g:+11,d} "
                  f"{arm_ok:7d} {cpu_ok_g:7d} {arm_ok-cpu_ok_g:+11d}")

    table(lambda g: g[0], "modulation", ("QPSK", "16QAM", "64QAM", "256QAM"))
    table(lambda g: next(label for label, lo, hi in WIDTH_BUCKETS if lo <= g[1] <= hi),
          "width (PRB)", tuple(label for label, _, _ in WIDTH_BUCKETS))


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("--arm", action="append", required=True, metavar="NAME=CSV")
    parser.add_argument("--cpu-index", required=True)
    parser.add_argument("--corpus", required=True)
    args = parser.parse_args(argv)

    cpu_bits, cpu_ok = read_cpu_index(args.cpu_index)
    universal = sorted(cpu_bits)

    arms = []
    for spec in args.arm:
        if "=" not in spec:
            parser.error(f"--arm wants NAME=CSV, got {spec!r}")
        name, path = spec.split("=", 1)
        bits, ok = read_arm(path)
        missing = [u for u in universal if u not in bits]
        if missing:
            print(f"REFUSING: {name} is missing {len(missing)} unit(s), first {missing[0]}", file=sys.stderr)
            return 2
        arms.append((name, bits, ok))

    geometry = read_sidecar_geometry(args.corpus, universal)
    for name, bits, ok in arms:
        summarise(name, bits, ok, universal, cpu_bits, cpu_ok)
        breakdown(bits, cpu_bits, ok, cpu_ok, geometry, universal)

    if len(arms) == 2:
        (name_a, bits_a, ok_a), (name_b, bits_b, ok_b) = arms
        total_a = sum(bits_a[u] for u in universal)
        total_b = sum(bits_b[u] for u in universal)
        diffs = [bits_a[u] - bits_b[u] for u in universal]
        lo, hi = bootstrap_ci(diffs)
        x, y, p = mcnemar(ok_a, ok_b, universal)
        print(f"== {name_a} - {name_b} ==")
        print(f"  {total_a-total_b:+,} ({100.0*(total_a-total_b)/total_b:+.2f}%)  "
              f"CI [{lo:+,}, {hi:+,}]  McNemar {x} : {y}  p = {p:.3g}")
        wins = sorted(universal, key=lambda u: -(bits_a[u] - bits_b[u]))[:8]
        print("  largest wins: " + ", ".join(
            f"{u}({bits_a[u]-bits_b[u]:+d})" for u in wins if bits_a[u] != bits_b[u]))
        losses = sorted(universal, key=lambda u: (bits_a[u] - bits_b[u]))[:5]
        print("  largest losses: " + ", ".join(
            f"{u}({bits_a[u]-bits_b[u]:+d})" for u in losses if bits_a[u] != bits_b[u]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
