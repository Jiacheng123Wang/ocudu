#!/usr/bin/env python3
"""Add the chain's LDPC iteration count to mother-set files that were built before the field existed.

WHY THIS IS A BACKFILL AND NOT A REBUILD. The mother set is 332 GB across 823 files; rebuilding it to add one integer
per sample would rewrite all of it. The dump index already holds the number (`iterations`), the dataset already holds the
unit names, and a zip member can be appended without touching the members that are already there -- so each file gains a
few kilobytes of work instead of a full rewrite.

WHERE THE INDEX COMES FROM. The local dump tree first, because it is faster and always current; when a leg's dump is no
longer on the machine, the archived tarball is read with `tar -xO`, i.e. streamed out of the archive without unpacking
it. Nothing is extracted to disk.

THE FIELD. -1 means "the chain never recorded a count" (a synthetic sample, or a leg whose index predates the column);
0 means "no decode" (a KO unit), which is a real value and must not be confused with unknown.

Usage:
    backfill_iterations.py <mother set dir> [--dump-root DIR] [--tarball-dir DIR] [--limit N] [--dry-run]
"""
import argparse
import collections
import csv
import io
import os
import subprocess
import sys
import zipfile

import numpy as np

MODS = ("QPSK", "16QAM", "64QAM", "256QAM")


def leg_and_population(filename):
    """`<leg>_<MOD>_<self|harq>.npz` -> (leg, population)."""
    stem = filename[:-4]
    for mod in MODS:
        for pop in ("self", "harq"):
            suffix = f"_{mod}_{pop}"
            if stem.endswith(suffix):
                return stem[:-len(suffix)], pop
    return None, None


def read_index_rows(text):
    """unit -> iterations, from a dump index.csv's text."""
    out = {}
    reader = csv.DictReader(io.StringIO(text))
    for row in reader:
        raw = row.get("iterations") or ""
        out[row["unit"]] = int(raw) if raw.strip().isdigit() else 0
    return out


def iterations_for(leg, population, args):
    """The unit -> iterations map for one leg, from the local dump or the archived tarball."""
    name = "index_harq_only.csv" if population == "harq" else "index.csv"
    local = os.path.join(args.dump_root, f"tb_{leg}", name)
    if os.path.exists(local):
        return read_index_rows(open(local).read()), "local"
    tar = os.path.join(args.tarball_dir, f"tb_{leg}.tar.gz")
    if os.path.exists(tar):
        member = f"tb_{leg}/{name}"
        proc = subprocess.run(["tar", "-xOzf", tar, member], capture_output=True, text=True)
        if proc.returncode == 0 and proc.stdout:
            return read_index_rows(proc.stdout), "tarball"
    return None, "missing"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("directory")
    ap.add_argument("--dump-root", default=os.path.expanduser("~/ai_depth3/dump_s2_mcs"))
    ap.add_argument("--tarball-dir", default="/Volumes/CORPUS/s2_mcs_tb_tarballs")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    files = sorted(f for f in os.listdir(args.directory) if f.endswith(".npz"))
    if args.limit:
        files = files[:args.limit]
    cache = {}
    stats = collections.Counter()
    for i, name in enumerate(files, 1):
        path = os.path.join(args.directory, name)
        leg, pop = leg_and_population(name)
        if leg is None:
            print(f"skip (unparsable name): {name}")
            stats["bad-name"] += 1
            continue
        key = (leg, pop)
        if key not in cache:
            cache[key] = iterations_for(leg, pop, args)
        table, source = cache[key]
        if table is None:
            print(f"skip (no index): {name}")
            stats["no-index"] += 1
            continue
        with zipfile.ZipFile(path) as zf:
            if "iterations.npy" in zf.namelist():
                stats["already"] += 1
                continue
            units = np.load(io.BytesIO(zf.read("unit.npy")), allow_pickle=True)
        values = np.array([table.get(str(u).split("@")[0], -1) for u in units], dtype=np.int16)
        missing = int((values < 0).sum())
        if args.dry_run:
            print(f"{name}: {len(values)} samples, {missing} unmatched (index from {source})")
            stats["dry"] += 1
            continue
        buffer = io.BytesIO()
        np.save(buffer, values)
        # Append, do not rewrite: the zip's existing members are left exactly as they are, and only the central
        # directory is rewritten at the end.
        with zipfile.ZipFile(path, mode="a", compression=zipfile.ZIP_DEFLATED) as zf:
            zf.writestr("iterations.npy", buffer.getvalue())
        stats[f"written-{source}"] += 1
        if i % 50 == 0 or i == len(files):
            print(f"  {i}/{len(files)}: {dict(stats)}", flush=True)
    print(f"done: {dict(stats)}")


if __name__ == "__main__":
    sys.exit(main())
