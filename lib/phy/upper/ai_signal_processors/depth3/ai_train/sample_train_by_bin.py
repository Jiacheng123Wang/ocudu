#!/usr/bin/env python3
"""Draw a quota-limited training set out of the assembled mother set, one quota per bin per population.

WHY PER BIN AND NOT PER FILE. The mother set is wildly unbalanced: a hundred-thousand-sample leg is mostly one cell,
and the common cells carry tens of thousands of samples while the rare ones carry a handful. Capping files would let
the big files spend the whole budget inside their favourite cell. Capping bins spends a fixed budget per cell, so
every cell contributes at most its quota and the thin cells contribute everything they have.

BIN = modulation x width band x SNR band, with the same edges as the inventory and the stratified split:
  modulation  QPSK / 16QAM / 64QAM / 256QAM   (from the file name and the qm column)
  width       <=3 / <=8 / <=24 / >24 PRBs      (from alloc_nof_prb)
  SNR         <0 / 0-6 / 6-10 / 10-14 / 14-20 / 20-30 / >=30 dB   (from snr_nominal)
POPULATION is self (first-transmission successes) or harq (recovered by retransmission), taken from the file name.

Each bin's self and harq populations get SEPARATE quotas. A bin whose population is smaller than its quota contributes
all of it, which is what "draw everything when there is not enough" means in practice.

The output is one npz per (modulation, population) carrying the same keys and dtypes as the inputs, with the `unit`
column preserved so every drawn sample can still be traced to the capture it came from, and a manifest recording the
quotas, the seed and the per-bin draw so the selection can be reproduced.

Usage:
  sample_train_by_bin.py --dir <mother dir> --out-dir <dir> [--quota-self 2500] [--quota-harq 250]
                         [--seed 20261031] [--name-prefix w2_train]
"""
import argparse
import collections
import json
import os
import re
import sys

import numpy as np

MODS = {2: "QPSK", 4: "16QAM", 6: "64QAM", 8: "256QAM"}
WIDTH_EDGES = [3, 8, 24]
SNR_EDGES = [0.0, 6.0, 10.0, 14.0, 20.0, 30.0]

# EVERY KEY THE BUILDER WRITES. An earlier version of this list omitted pilot_ls_ratio and silently dropped it from
# every output file -- the kind of omission that is invisible until someone needs the column. When the builder gains a
# key this list has to gain it too; the loader below refuses to run if it meets an input key that is not copied.
FIELD_ORDER = ["X", "H", "r", "sigma2_over_r2", "bits", "nof_bits", "tb_bits", "nof_tb_bits",
               "rnti", "n_id", "rv", "alloc_first_prb", "alloc_nof_prb", "dmrs_bitmap",
               "qm", "snr_nominal", "source", "pilot_ls_ratio", "unit"]


def width_band(prb):
    for edge in WIDTH_EDGES:
        if prb <= edge:
            return "<={}".format(edge)
    return ">{}".format(WIDTH_EDGES[-1])


def snr_band(snr):
    for edge in SNR_EDGES:
        if snr < edge:
            return "<{}".format(int(edge)) if edge == SNR_EDGES[0] else "{}-{}".format(int(prev), int(edge))
        prev = edge
    return ">={}".format(int(SNR_EDGES[-1]))


def population_of(path):
    base = os.path.basename(path)
    if base.endswith("_harq_only.npz"):
        return "harq"
    if base.endswith("_harq.npz"):
        return "harq"
    if base.endswith("_self.npz"):
        return "self"
    return None


def mod_of(path):
    base = os.path.basename(path)
    m = re.search(r"_(QPSK|16QAM|64QAM|256QAM)_(self|harq|harq_only)\.npz$", base)
    return m.group(1) if m else None


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True, help="the mother set directory")
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--quota-self", type=int, default=2500)
    ap.add_argument("--quota-harq", type=int, default=250)
    ap.add_argument("--seed", type=int, default=20261031)
    ap.add_argument("--name-prefix", default="w2_train")
    args = ap.parse_args(argv)

    rng = np.random.default_rng(args.seed)
    files = sorted(f for f in os.listdir(args.dir) if f.endswith(".npz"))
    print("{} file(s) in {}".format(len(files), args.dir))

    # ---- pass 1: how many samples per (bin, population), and where they live ------------------
    index = collections.defaultdict(list)   # (mod, width, snr, pop) -> [(file, row), ...]
    per_file = {}
    for f in files:
        pop = population_of(f)
        mod = mod_of(f)
        if pop is None or mod is None:
            print("  skip (unrecognised name): {}".format(f), file=sys.stderr)
            continue
        z = np.load(os.path.join(args.dir, f), allow_pickle=True)
        qm = z["qm"][:]
        prb = z["alloc_nof_prb"][:]
        snr = z["snr_nominal"][:]
        n = len(qm)
        per_file[f] = (n, pop, mod)
        for i in range(n):
            m = MODS.get(int(qm[i]))
            if m is None:
                continue
            key = (m, width_band(int(prb[i])), snr_band(float(snr[i])), pop)
            index[key].append((f, i))
        z.close()

    print("\npopulated bins: {}".format(len({(k[0], k[1], k[2]) for k in index})))
    for pop in ("self", "harq"):
        n = sum(len(v) for k, v in index.items() if k[3] == pop)
        print("  {} available: {:,}".format(pop, n))

    # ---- draw the quotas ---------------------------------------------------------------------
    quota = {"self": args.quota_self, "harq": args.quota_harq}
    chosen = collections.defaultdict(list)   # (mod, pop) -> [(file, row), ...]
    manifest = []
    for key in sorted(index):
        mod, width, snr, pop = key
        pool = index[key]
        take = pool if len(pool) <= quota[pop] else [pool[i] for i in
                                                     rng.choice(len(pool), size=quota[pop], replace=False)]
        for item in take:
            chosen[(mod, pop)].append(item)
        manifest.append({"modulation": mod, "width": width, "snr": snr, "population": pop,
                         "available": len(pool), "drawn": len(take), "quota": quota[pop]})

    drawn_total = sum(len(v) for v in chosen.values())
    print("\ndrawn: {:,} sample(s)".format(drawn_total))

    # ---- write one npz per (modulation, population) -------------------------------------------
    os.makedirs(args.out_dir, exist_ok=True)
    written = []
    for (mod, pop), rows in sorted(chosen.items()):
        # Group by source file so each file is opened once.
        by_file = collections.defaultdict(list)
        for f, i in rows:
            by_file[f].append(i)
        parts = {k: [] for k in FIELD_ORDER}
        for f in sorted(by_file):
            z = np.load(os.path.join(args.dir, f), allow_pickle=True)
            unknown = [k for k in z.files if k not in FIELD_ORDER]
            if unknown:
                raise SystemExit("input key(s) not in FIELD_ORDER, refusing to drop them silently: "
                                 + ", ".join(sorted(unknown)))
            idx = np.array(sorted(by_file[f]), dtype=np.int64)
            for k in FIELD_ORDER:
                if k not in z.files:
                    continue
                parts[k].append(z[k][idx])
            z.close()
        out = {}
        for k, chunks in parts.items():
            if not chunks:
                continue
            if k == "unit":
                merged = []
                for c in chunks:
                    merged.extend(list(c))
                out[k] = np.array(merged, dtype=object)
            elif k in ("bits", "tb_bits"):
                # WIDTH VARIES BY FILE. These are packed byte rows whose second dimension is the widest codeword
                # in THAT file, so two files disagree as soon as their largest transport block differs (measured:
                # 3312 against 3672 bytes). Pad the narrow ones with zeros -- the valid length is nof_bits, which is
                # carried per sample, so padding never changes what a reader sees.
                width = max(c.shape[1] for c in chunks)
                chunks = [c if c.shape[1] == width else
                          np.pad(c, ((0, 0), (0, width - c.shape[1])), mode="constant")
                          for c in chunks]
                out[k] = np.concatenate(chunks, axis=0)
            else:
                out[k] = np.concatenate(chunks, axis=0)
        name = "{}_{}_{}.npz".format(args.name_prefix, mod, pop)
        path = os.path.join(args.out_dir, name)
        np.savez(path, **out)
        n = len(out["qm"])
        written.append({"file": name, "samples": n})
        print("  wrote {}: {:,} samples".format(name, n))

    man = {"dir": os.path.abspath(args.dir), "quota_self": args.quota_self,
           "quota_harq": args.quota_harq, "seed": args.seed,
           "drawn_total": drawn_total, "written": written, "bins": manifest}
    with open(os.path.join(args.out_dir, "manifest_{}.json".format(args.name_prefix)), "w") as fh:
        json.dump(man, fh, indent=1, sort_keys=True)
    print("\nwrote manifest_{}.json".format(args.name_prefix))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
