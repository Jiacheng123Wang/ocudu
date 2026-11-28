#!/usr/bin/env python3
"""Replace `snr_nominal` in built npz with the dump's own `sinr_db`.

WHY. Until 2026-11-28 the builder filled `snr_nominal` from the capture's `_ce.txt`, a second
estimator of the same quantity. On the legs measured so far it agrees with the dump within 3 dB on
92% of samples, but its 5th percentile is 9 dB off and on 0.01-2.4% of samples it goes negative --
impossible for a CRC-OK block, and enough to put a 22 dB sample into the "-3 dB" bin. The dump's
`index.csv:sinr_db` is the chain's own per-reception SINR, present for every row, and is what every
comparison already uses.

HOW. Same trick as clamp_iterations.py: the corrected array is written as `snr_nominal.npy` and
`zip` replaces that one member in place, copying X and H without decompressing them.

Usage:
  refill_snr.py --out DIR --dump-root DIR [--leg LEG ...] [--dry-run]
With no --leg it repairs every leg marked `ok` in <out>/_progress.csv, which is exactly the set no
build is currently writing.
"""
import argparse
import csv
import glob
import os
import subprocess
import sys
import tempfile

import numpy as np


def index_sinr(path):
    """unit -> sinr_db (float) from a dump index."""
    table = {}
    with open(path) as handle:
        for row in csv.DictReader(handle):
            try:
                table[row["unit"]] = float(row["sinr_db"])
            except (KeyError, ValueError):
                table[row["unit"]] = float("nan")
    return table


def repair_file(path, dump, dry_run):
    """Return (n, changed). n is the number of samples whose value actually moved."""
    pop = "harq" if path.endswith("_harq.npz") else "self"
    index = os.path.join(dump, "index_harq_only.csv" if pop == "harq" else "index_self.csv")
    if not os.path.exists(index):
        raise SystemExit(f"{path}: no {index}")
    sinr = index_sinr(index)

    with np.load(path, allow_pickle=True) as data:
        units = np.asarray(data["unit"])
        before = np.asarray(data["snr_nominal"], dtype=np.float32)
        members = list(data.files)

    after = np.empty(len(units), dtype=np.float32)
    missing = []
    for i, unit in enumerate(units):
        key = str(unit).split("@")[0]
        if key not in sinr:
            missing.append(key)
            after[i] = float("nan")
        else:
            after[i] = np.float32(sinr[key])
    if missing:
        raise SystemExit(f"{path}: {len(missing)} unit(s) not in the dump index, e.g. {missing[:3]}")

    moved = int(np.sum(after != before)) if not np.isnan(after).any() else len(units)
    if dry_run or moved == 0:
        return moved, False

    with tempfile.TemporaryDirectory() as tmp:
        np.save(os.path.join(tmp, "snr_nominal.npy"), after)
        subprocess.run(["zip", "-q", os.path.abspath(path), "snr_nominal.npy"], cwd=tmp, check=True)

    with np.load(path, allow_pickle=True) as data:
        if sorted(data.files) != sorted(members):
            raise RuntimeError(f"{path}: member list changed")
        check = np.asarray(data["snr_nominal"], dtype=np.float32)
        data["X"]  # the big member must still be readable
    if not np.allclose(check, after, equal_nan=True):
        raise RuntimeError(f"{path}: repair did not take")
    return moved, True


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True, help="mother-set directory holding the npz")
    ap.add_argument("--dump-root", required=True, help="directory holding tb_<leg>/")
    ap.add_argument("--leg", action="append", default=[], help="leg(s); default: every ok leg")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    legs = list(args.leg)
    if not legs:
        progress = os.path.join(args.out, "_progress.csv")
        with open(progress) as handle:
            legs = [r["leg"] for r in csv.DictReader(handle) if r["status"] == "ok"]
    if not legs:
        sys.exit("no legs to repair")

    total_moved = total_files = 0
    for leg in legs:
        dump = os.path.join(args.dump_root, "tb_" + leg)
        files = sorted(glob.glob(os.path.join(args.out, leg + "_*.npz")))
        if not files:
            print(f"{leg}: no npz, skipped")
            continue
        for path in files:
            moved, changed = repair_file(path, dump, args.dry_run)
            total_moved += moved
            total_files += 1 if changed else 0
            if changed or args.dry_run:
                print(f"{os.path.basename(path):52s} {moved:6d} sample(s) "
                      f"{'would change' if args.dry_run else 'rewritten'}")
    print(f"{'would rewrite' if args.dry_run else 'rewrote'} {total_files} file(s), "
          f"{total_moved} sample value(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
