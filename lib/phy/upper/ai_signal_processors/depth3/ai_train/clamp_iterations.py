#!/usr/bin/env python3
"""Floor `iterations`/`iterations_max` at one in mother-set npz that carry the old zero values.

WHY. The replay cast its float mean iteration count to unsigned, so a decode whose mean fell in
[0,1) was written as 0 -- the same value the dump uses for "no decode". The replay is fixed, but
the dumps taken before 2026-11-27 still carry the zeros, and a mother set built from them inherits
them: 3 481 samples across the first fifteen legs, up to 2.7% of one leg. This tool repairs the
npz IN PLACE. A mother set that is rebuilt from a dump after the fix needs nothing.

HOW. `zip` is used to replace the two members inside the archive. It copies every other member --
including the 1.3 GB `X` -- without decompressing it, which is what makes the repair take seconds
instead of minutes per file. The archive is verified afterwards: the member list is unchanged, the
lengths are unchanged, and no zero is left.

Usage: clamp_iterations.py --dir DIR [--dry-run]
       clamp_iterations.py --file A.npz [--file B.npz ...]
"""
import argparse
import glob
import os
import subprocess
import sys
import tempfile

import numpy as np

WANTED = ("X", "H", "bits", "tb_bits", "iterations", "iterations_max", "unit")


def repair(path, dry_run):
    """Return (zeros_in_iterations, zeros_in_max, changed)."""
    with np.load(path, allow_pickle=True) as data:
        members = list(data.files)
        iterations = np.array(data["iterations"], copy=True)
        iterations_max = np.array(data["iterations_max"], copy=True)
    zeros = int((iterations == 0).sum())
    zeros_max = int((iterations_max == 0).sum())
    if zeros == 0 and zeros_max == 0:
        return 0, 0, False
    if dry_run:
        return zeros, zeros_max, True

    iterations[iterations == 0] = 1
    iterations_max[iterations_max == 0] = 1
    with tempfile.TemporaryDirectory() as tmp:
        np.save(os.path.join(tmp, "iterations.npy"), iterations)
        np.save(os.path.join(tmp, "iterations_max.npy"), iterations_max)
        # The archive path must be absolute: the command runs with cwd set to the temp directory so
        # that the member names come out as plain `iterations.npy`.
        subprocess.run(["zip", "-q", os.path.abspath(path), "iterations.npy", "iterations_max.npy"],
                       cwd=tmp, check=True)

    with np.load(path, allow_pickle=True) as data:
        after = list(data.files)
        fixed = np.asarray(data["iterations"])
        fixed_max = np.asarray(data["iterations_max"])
        for key in WANTED:
            data[key]  # every member must still be readable, X and H included
    if sorted(after) != sorted(members):
        raise RuntimeError(f"{path}: member list changed by the repair")
    if len(fixed) != len(iterations) or int((fixed == 0).sum()) or int((fixed_max == 0).sum()):
        raise RuntimeError(f"{path}: repair did not take")
    return zeros, zeros_max, True


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", default="")
    ap.add_argument("--file", action="append", default=[])
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    paths = list(args.file)
    if args.dir:
        paths += sorted(glob.glob(os.path.join(args.dir, "*.npz")))
    if not paths:
        sys.exit("nothing to do: pass --dir or --file")

    total = files = 0
    for path in paths:
        zeros, zeros_max, changed = repair(path, args.dry_run)
        if not changed:
            continue
        files += 1
        total += zeros + zeros_max
        print(f"{os.path.basename(path):48s} iterations=0: {zeros:4d}  iterations_max=0: {zeros_max:3d}"
              f"{'  (dry run)' if args.dry_run else '  -> floored at 1'}")
    verb = "would repair" if args.dry_run else "repaired"
    print(f"{verb} {files} file(s), {total} zero value(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
