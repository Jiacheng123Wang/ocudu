#!/usr/bin/env python3
"""Draw random sample sets out of the mother set, optionally excluding samples by capture name.

WHAT THE DISJOINTNESS GUARANTEE ACTUALLY COVERS. Rows are drawn without replacement, so two sets made in one call never
share a row. They can still share a CAPTURE: the mother set holds 2,526,019 rows but only 1,914,144 distinct capture
names, because one reception can appear more than once -- as a first-transmission success and again as a recovered
sample, or in files built at different times. Drawing 20,000 rows and splitting them in half therefore yields about
forty captures present in both halves; measured 40 on the w2 draw against a null of 43.8. That is chance, not a fault,
but it means the guarantee is ROW-LEVEL and must not be described as capture-level.

The `--exclude` argument buys the stronger property for the training set: every row whose capture name appears there is
removed before drawing. Note what it costs. A bin whose whole population went into training then has no test samples at
all, so the test set goes blind to exactly the rare bins that most need measuring -- on the w2 corpus that cost was
judged too high and the flag was left off. It is here for corpora rich enough to afford it.
"""
import argparse
import json
import os
import sys

import numpy as np

FIELD_ORDER = ["X", "H", "r", "sigma2_over_r2", "bits", "nof_bits", "tb_bits", "nof_tb_bits",
               "rnti", "n_id", "rv", "alloc_first_prb", "alloc_nof_prb", "dmrs_bitmap",
               "qm", "snr_nominal", "source", "pilot_ls_ratio",
               # The control payload, carried through so a drawn subset keeps the element map its
               # label plane is built from. A file that predates the fields simply has none.
               "uci_nof_harq_ack", "uci_ack_re", "uci_ack_rvd_re", "uci_csi1_re",
               "unit"]

#: Fields a file may predate. A dataset built before the control payload was carried has no
#: control information in its element map, so zeros are the true value for those rows.
OPTIONAL_FIELDS = {"uci_nof_harq_ack", "uci_ack_re", "uci_ack_rvd_re", "uci_csi1_re"}


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
    ap.add_argument("--no-cache", action="store_true",
                    help="ignore and do not write the per-file unit-name cache")
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
    # THE UNIT COLUMN IS THE EXPENSIVE PART, so it is cached. It is a pickled object array, and unpickling two and a
    # half million strings out of 220 npz files took about fifteen minutes per pass (measured: a two-thousand-sample
    # draw ran thirty-one minutes). The cache is one npy per source file holding just the names, keyed by the source's
    # size and mtime so a rebuilt dataset invalidates it rather than serving stale names.
    cache_dir = os.path.join(args.out_dir, ".unit_cache")
    if not args.no_cache:
        os.makedirs(cache_dir, exist_ok=True)
    rows = []      # (file, row)
    for f in files:
        src = os.path.join(args.dir, f)
        st = os.stat(src)
        cache = os.path.join(cache_dir, f.replace(".npz", ".npy"))
        units = None
        if not args.no_cache and os.path.exists(cache):
            try:
                cached = np.load(cache, allow_pickle=False)
                if int(cached[0]) == st.st_size and int(cached[1]) == int(st.st_mtime):
                    units = [str(u) for u in cached[2:]]
            except Exception:
                units = None
        if units is None:
            z = np.load(src, allow_pickle=True)
            units = [str(u) for u in z["unit"]]
            z.close()
            if not args.no_cache:
                try:
                    np.save(cache, np.array([st.st_size, int(st.st_mtime)] + units, dtype=object),
                            allow_pickle=True)
                except Exception:
                    pass
        for i, u in enumerate(units):
            if u not in excluded:
                rows.append((f, i))
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
                if k in z.files:
                    chunks[k].append(z[k][idx])
                elif k in OPTIONAL_FIELDS:
                    chunks[k].append(np.zeros(idx.size, dtype=np.int32))
                else:
                    raise SystemExit(f"input {f} has no '{k}' column")
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
