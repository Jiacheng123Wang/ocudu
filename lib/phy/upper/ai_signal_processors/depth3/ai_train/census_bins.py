#!/usr/bin/env python3
"""Census a capture corpus into modulation x width band x SNR band bins.

WHY IT LIVES HERE. It started as a throwaway in /tmp, and /tmp is wiped on reboot -- the user's rule is that anything worth reusing
goes into the workflow's own tree. This is the tool that answers "what does the corpus actually cover", which is the question every
collection decision now starts from.

The bins are the same ones select_by_bin.py and split_depth3_stratified.py use, so the three agree by construction.

Usage: census_bins.py [--jobs N] [--csv OUT] <leg dir> [<leg dir> ...]
"""
import argparse
import collections
import glob
import math
import os
from multiprocessing import Pool

WIDTH_EDGES = [3, 8, 24]
SNR_EDGES = [0.0, 6.0, 10.0, 14.0, 20.0, 30.0]


def width_band(prb):
    for edge in WIDTH_EDGES:
        if prb <= edge:
            return f"<={edge}"
    return f">{WIDTH_EDGES[-1]}"


def snr_band(snr_db):
    previous = None
    for edge in SNR_EDGES:
        if snr_db < edge:
            return f"<{edge:g}" if previous is None else f"{previous:g}-{edge:g}"
        previous = edge
    return f">={SNR_EDGES[-1]:g}"


def one_capture(path):
    """The bin of one capture, or None when its sidecars do not name one."""
    leg = os.path.basename(os.path.dirname(path))
    mod = prb = None
    try:
        with open(path, errors="ignore") as handle:
            for line in handle:
                key, sep, value = line.strip().partition("=")
                if not sep:
                    continue
                if key == "modulation":
                    mod = value
                elif key == "alloc_nof_rb":
                    try:
                        prb = int(value)
                    except ValueError:
                        prb = None
                if mod is not None and prb is not None:
                    break
    except OSError:
        return None
    if mod is None or prb is None:
        return None
    linear = None
    try:
        with open(path[:-4] + "_ce.txt", errors="ignore") as handle:
            for line in handle:
                if line.startswith("port=0"):
                    for token in line.split():
                        if token.startswith("snr="):
                            try:
                                linear = float(token[4:])
                            except ValueError:
                                linear = None
                    break
    except OSError:
        return None
    if linear is None or linear <= 0:
        return None
    return leg, (mod, width_band(prb), snr_band(10 * math.log10(linear)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--jobs", type=int, default=14)
    ap.add_argument("--csv", default="")
    ap.add_argument("legs", nargs="+")
    args = ap.parse_args()

    files = []
    for leg in args.legs:
        files += [f for f in glob.glob(os.path.join(leg, "cap_*.txt")) if not f.endswith("_ce.txt")]
    print(f"{len(files)} capture(s) over {len(args.legs)} leg(s)", flush=True)

    per_leg = collections.Counter()
    total = collections.Counter()
    done = 0
    with Pool(args.jobs) as pool:
        for result in pool.imap_unordered(one_capture, files, chunksize=200):
            done += 1
            if result is not None:
                per_leg[result] += 1
                total[result[1]] += 1
            if done % 100000 == 0:
                print(f"  {done}/{len(files)}", flush=True)

    if args.csv:
        with open(args.csv, "w") as handle:
            handle.write("leg,modulation,width,snr,n\n")
            for (leg, (mod, width, snr)), n in sorted(per_leg.items()):
                handle.write(f"{leg},{mod},{width},{snr},{n}\n")
            for (mod, width, snr), n in sorted(total.items()):
                handle.write(f"ALL,{mod},{width},{snr},{n}\n")
        print(f"wrote {args.csv}")

    print(f"bins: {len(total)}  samples: {sum(total.values())}")
    print("--- most populated ---")
    for key, count in sorted(total.items(), key=lambda kv: -kv[1])[:15]:
        print(f"  {key[0]:6s} {key[1]:5s} {key[2]:7s} {count:7d}")
    print("--- rare (<100 samples) ---")
    for key, count in sorted(total.items(), key=lambda kv: kv[1]):
        if count < 100:
            print(f"  {key[0]:6s} {key[1]:5s} {key[2]:7s} {count:7d}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
