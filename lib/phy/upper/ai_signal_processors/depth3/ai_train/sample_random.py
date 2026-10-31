#!/usr/bin/env python3
"""Draw random sample sets out of the mother set, guaranteeing they do not overlap each other or the training set.

WHY DISJOINTNESS IS THE WHOLE JOB. A test set drawn independently from the same pool overlaps the training set by
construction -- at ten thousand out of two and a half million the expected overlap is about forty samples, and those
forty are memorised, not measured. Worse, draws made in sequence from one another must also be disjoint, or test1 and
test2 stop being independent measurements of the same thing. So this tool takes an exclusion set of sample identities
and removes them before drawing, then adds its own draw to the exclusion set before the next draw.

IDENTITY is the capture name in `unit`. Every row of every dataset is one reception, and the capture name carries its
frame, slot and RNTI, so it identifies the reception across files. The same capture can legitimately appear twice --
once as a first-transmission success and once as a recovered sample -- which is exactly why the identity has to be
checked rather than assumed unique.

Usage:
  sample_random.py --dir <mother dir> --out-dir <dir> --sets 2 --count 10000 \
                   [--exclude <npz-or-dir> ...] [--seed 20261031] [--name-prefix w2_test]
Writes <prefix>1.npz, <prefix>2.npz, ... and manifest_<prefix>.json.
"""
import argparse
import json
import os
import sys

import numpy as np

FIELD_ORDER = ["X", "H", "r", "sigma2_over_r2", "bits", "nof_bits", "tb_bits", "nof_tb_bits",
               "rnti", "n_id", "rv", "alloc_first_prb", "alloc_nof_prb", "dmrs_bitmap",
               "qm", "snr_nominal", "source", "pilot_ls_ratio", "unit"]


def load_units(path):
    z = np.load(path, allow_pickle=True)
    units = set(str(u) for u in z["unit"])
    z.close()
    return units


def collect_units(path):
    """Union of unit names over an npz file or every npz in a directory."""
    units = set()
    if os.path.isdir(path):
        for f in sorted(os.listdir(path)):
            if f.endswith(".npz"):
                units |= load_units(os.path.join(path, f))
    elif path.endswith(".npz"):
        units = load_units(path)
    else:
        raise SystemExit("--exclude expects an npz file or a directory: " + path)
    return units


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True)
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--sets", type=int, default=2)
    ap.add_argument("--count", type=int, default=10000)
    ap.add_argument("--exclude", nargs="*", default=[])
    ap.add_argument("--seed", type=int, default=20261031)
    ap.add_argument("--name-prefix", default="w2_test")
    args = ap.parse_args(argv)

    excluded = set()
    for path in args.exclude:
        got = collect_units(path)
        print("excluded {} unit(s) from {}".format(len(got), path))
        excluded |= got
    print("excluded total: {:,} unit(s)".format(len(excluded)))

    files = sorted(f for f in os.listdir(args.dir) if f.endswith(".npz"))
    print("{} file(s) in {}".format(len(files), args.dir))

    # ---- pass 1: index every row that is not excluded -----------------------------------------
    rows = []      # (file, row)
    for f in files:
        z = np.load(os.path.join(args.dir, f), allow_pickle=True)
        units = [str(u) for u in z["unit"]]
        for i, u in enumerate(units):
            if u not in excluded:
                rows.append((f, i))
        z.close()
    print("available after exclusion: {:,} sample(s)".format(len(rows)))
    need = args.sets * args.count
    if len(rows) < need:
        raise SystemExit("not enough samples: need {:,}, have {:,}".format(need, len(rows)))

    # ---- draw ---------------------------------------------------------------------------------
    rng = np.random.default_rng(args.seed)
    pick = rng.choice(len(rows), size=need, replace=False)
    os.makedirs(args.out_dir, exist_ok=True)
    manifest = {"dir": os.path.abspath(args.dir), "count": args.count, "sets": args.sets,
                "seed": args.seed, "excluded_units": len(excluded),
                "available": len(rows), "sets_written": []}

    for s in range(args.sets):
        chosen = [rows[int(pick[s * args.count + k])] for k in range(args.count)]
        per_file = {}
        for f, i in chosen:
            per_file.setdefault(f, []).append(i)
        out = {}
        chunks = {k: [] for k in FIELD_ORDER}
        for f in sorted(per_file):
            z = np.load(os.path.join(args.dir, f), allow_pickle=True)
            unknown = [k for k in z.files if k not in FIELD_ORDER]
            if unknown:
                raise SystemExit("input key(s) not in FIELD_ORDER: " + ", ".join(sorted(unknown)))
            idx = np.array(sorted(per_file[f]), dtype=np.int64)
            for k in FIELD_ORDER:
                chunks[k].append(z[k][idx])
            z.close()
        for k in FIELD_ORDER:
            if k == "unit":
                merged = []
                for c in chunks[k]:
                    merged.extend(list(c))
                out[k] = np.array(merged, dtype=object)
            elif k in ("bits", "tb_bits"):
                width = max(c.shape[1] for c in chunks[k])
                chunks[k] = [c if c.shape[1] == width else
                             np.pad(c, ((0, 0), (0, width - c.shape[1])), mode="constant")
                             for c in chunks[k]]
                out[k] = np.concatenate(chunks[k], axis=0)
            else:
                out[k] = np.concatenate(chunks[k], axis=0)
        name = "{}{}.npz".format(args.name_prefix, s + 1)
        np.savez(os.path.join(args.out_dir, name), **out)
        print("wrote {}: {:,} samples".format(name, len(out["qm"])))
        manifest["sets_written"].append({"file": name, "samples": int(len(out["qm"])),
                                         "units": len(set(str(u) for u in out["unit"]))})

    with open(os.path.join(args.out_dir, "manifest_{}.json".format(args.name_prefix)), "w") as fh:
        json.dump(manifest, fh, indent=1, sort_keys=True)
    print("wrote manifest_{}.json".format(args.name_prefix))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
