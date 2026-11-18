"""Compare the sets of units each arm decodes (CRC OK) on the 500-unit probe.

WHY SETS AND NOT A SINGLE TOTAL. Two arms can deliver the same number of bits while agreeing on almost nothing --
one is better in the wide band, the other in the narrow -- and the total hides that. The set structure says which
regions each arm owns, and therefore whether an ensemble could in principle help at all: if one arm's OK set is a
strict superset of the others', nothing can be gained by combining them.

Usage:
    python compare_probe_sets.py <arm=path.csv> [<arm=path.csv> ...] [--reference PATH_TO_CPU_INDEX]
"""
import argparse
import collections
import csv
import glob
import os

import numpy as np

HOME = os.path.expanduser("~")
BASE = os.path.join(HOME, "ai_depth3/dump_s2_amc/tb_s2_amc_ping_1102a")
RES = os.path.join(HOME, "ai_depth3/datasets_eval_reserved_1102a")
PROBE = os.path.join(HOME, "ai_depth3/exp_bits_1102/units_probe_500.txt")


def load_ok(path, want):
    rows = list(csv.DictReader(open(path)))
    return {r["unit"] for r in rows if r.get("status") == "OK" and r["unit"] in want}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("arms", nargs="+", help="name=path/to/probe.csv")
    args = ap.parse_args()
    want = {l.strip() for l in open(PROBE) if l.strip()}
    units = {a.split("=", 1)[0]: load_ok(os.path.expanduser(a.split("=", 1)[1]), want) for a in args.arms}
    names = list(units)
    print(f"probe units {len(want)};  " + "  ".join(f"{n}: {len(units[n])}" for n in names))

    # pairwise overlap
    print("\npairwise |A ∩ B| / |A ∪ B| (Jaccard):")
    for i, a in enumerate(names):
        for b in names[i + 1:]:
            inter = len(units[a] & units[b]); union = len(units[a] | units[b])
            print(f"  {a:10} vs {b:10}: {inter:3d} / {union:3d}  = {inter/union:.3f}")

    # the 2^n pattern table: who decodes this unit
    print("\npattern (which arms decode it) -> number of units:")
    count = collections.Counter()
    for u in set().union(*units.values()):
        key = "".join("1" if u in units[n] else "0" for n in names)
        count[key] += 1
    print("  " + "  ".join(names))
    for key, n in sorted(count.items(), key=lambda kv: -kv[1]):
        print(f"  {key}   {n:3d}")

    # exclusive ownership, with the units named
    for n in names:
        others = set().union(*[units[m] for m in names if m != n])
        excl = units[n] - others
        print(f"\n{n:10} exclusive: {len(excl)} unit(s)" + (": " + ", ".join(sorted(excl)) if excl else ""))

    # attributes of the exclusive units, when the metadata is there
    meta = {}
    for f in glob.glob(os.path.join(RES, "s2_amc_ping_1102a_*.npz")):
        with np.load(f, allow_pickle=True) as z:
            u = [str(x).split("@")[0] for x in z["unit"]]
            prb = np.asarray(z["alloc_nof_prb"]); qm = np.asarray(z["qm"]); snr = np.asarray(z["snr_nominal"], dtype="f8")
            mods = {2: "QPSK", 4: "16QAM", 6: "64QAM", 8: "256QAM"}
            for i, unit in enumerate(u):
                meta[unit] = (mods.get(int(qm[i]), "?"), int(prb[i]), round(float(snr[i]), 1))
    if meta:
        for n in names:
            others = set().union(*[units[m] for m in names if m != n])
            for u in sorted(units[n] - others):
                print(f"    {n:10} exclusive {u:28} {meta.get(u)}")


if __name__ == "__main__":
    main()
