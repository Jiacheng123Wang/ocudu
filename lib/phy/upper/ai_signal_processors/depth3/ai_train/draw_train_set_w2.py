#!/usr/bin/env python3
"""Draw a per-bin capped training set from the w2 mother set and write it memory-mappable.

WHAT IT DOES. Reads every `.npz` in a mother-set directory, assigns each SAMPLE to one of the 112 bins
the project uses (modulation x width band x SNR band), decides `self` or `harq` from the file it came
from, and keeps a bounded random draw from each bin. Writes the result as one `.npy` per array so the
trainer can open it memory-mapped (`train_depth3.py` accepts a directory since 2026-11-03).

WHY PER-BIN RATHER THAN A FLAT RANDOM SAMPLE. A flat draw reproduces the corpus's own mixture, which is
dominated by whichever legs were longest and whichever allocations the scheduler happened to favour.
The bins are what the model has to be good at, so the draw is stratified: every bin contributes, and
none of them can swamp the rest. A bin below its cap contributes everything it has.

THE CAPS (`corpus_capture_and_new_datasets_build.md` section 10.2, user instruction 2026-11-03):
self 2 500 and harq 250 per bin. The harq cap is a tenth of the self cap because recovered samples
carry a donor dependency -- their label comes from another reception in the same chain -- so they are
worth having but not worth letting dominate a bin.

WIDTH AND SNR BANDS. Width comes from `alloc_nof_prb` and uses the same edges the replay test builder
does: <=3, <=8, <=24, >24. SNR comes from `snr_nominal` with the project's edges: <0, 0-6, 6-10,
10-14, 14-20, 20-30, >=30. Same edges as section 10.2's table so the report can be read against it.

USAGE.
  draw_train_set_w2.py --in ~/work/ai_phy_train/data/datasets_full_w2 \
                       --out ~/work/ai_phy_train/data/datasets_train_w2 \
                       --self-cap 2500 --harq-cap 250 --seed 20261103
"""
import argparse
import collections
import glob
import os
import sys

import numpy as np

# Kept in step with `Batches.ARRAYS` / `Batches.UCI_ARRAYS` in train_depth3.py.
ARRAYS = ("X", "H", "bits", "nof_bits", "alloc_first_prb", "alloc_nof_prb", "dmrs_bitmap",
          "qm", "sigma2_over_r2", "snr_nominal", "tb_bits", "nof_tb_bits", "rnti", "n_id",
          "rv")
OPTIONAL = ("uci_nof_harq_ack", "uci_ack_re", "uci_ack_rvd_re", "uci_csi1_re")

QM_NAME = {2: "QPSK", 4: "16QAM", 6: "64QAM", 8: "256QAM"}
WIDTH_EDGES = ((3, "<=3"), (8, "<=8"), (24, "<=24"))
SNR_EDGES = ((-1e9, 0.0, "<0"), (0.0, 6.0, "0-6"), (6.0, 10.0, "6-10"), (10.0, 14.0, "10-14"),
             (14.0, 20.0, "14-20"), (20.0, 30.0, "20-30"), (30.0, 1e9, ">=30"))


def width_band(nof_prb):
    for edge, name in WIDTH_EDGES:
        if nof_prb <= edge:
            return name
    return ">24"


def snr_band(snr_db):
    for lo, hi, name in SNR_EDGES:
        if lo <= snr_db < hi:
            return name
    return ">=30"


def is_harq(path):
    """`self` and `harq` are the two populations a leg produces, and the file name says which.

    A name that says neither is a build that predates the convention -- refused rather than guessed,
    because putting a recovered sample in the self population would understate the donor dependency
    the caps exist to bound.
    """
    base = os.path.basename(path)
    if "_self" in base:
        return False
    if "_harq" in base:
        return True
    raise ValueError(f"cannot tell self from harq in {base}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="src", required=True, help="mother-set directory of .npz files")
    ap.add_argument("--out", dest="dst", required=True, help="output directory for the .npy files")
    ap.add_argument("--self-cap", type=int, default=2500)
    ap.add_argument("--harq-cap", type=int, default=250)
    ap.add_argument("--seed", type=int, default=20261103)
    ap.add_argument("--dry-run", action="store_true", help="report the draw without writing anything")
    args = ap.parse_args()

    files = sorted(glob.glob(os.path.join(args.src, "*.npz")))
    if not files:
        print(f"FATAL: no .npz files under {args.src}", file=sys.stderr)
        return 1
    print(f"{len(files)} dataset file(s) under {args.src}")

    # ---- 1. bin every sample, remembering where it came from ---------------------------------------
    bins = collections.defaultdict(list)          # (qm, width, snr, harq) -> [(file_idx, sample_idx)]
    for fi, path in enumerate(files):
        harq = is_harq(path)
        with np.load(path, allow_pickle=False) as z:
            qm = z["qm"]
            prb = z["alloc_nof_prb"]
            snr = z["snr_nominal"]
            n = qm.shape[0]
            for i in range(n):
                key = (int(qm[i]), width_band(int(prb[i])), snr_band(float(snr[i])), harq)
                bins[key].append((fi, i))
    total = sum(len(v) for v in bins.values())
    print(f"{total} sample(s) across {len(bins)} non-empty bin(s)")

    # ---- 2. draw from each bin ---------------------------------------------------------------------
    rng = np.random.default_rng(args.seed)
    drawn = {}
    for key in sorted(bins):
        members = bins[key]
        cap = args.harq_cap if key[3] else args.self_cap
        if len(members) <= cap:
            drawn[key] = members
        else:
            pick = rng.choice(len(members), size=cap, replace=False)
            drawn[key] = [members[j] for j in sorted(pick)]

    kept = sum(len(v) for v in drawn.values())
    print(f"drew {kept} sample(s) ({kept / total * 100:.1f}% of the mother set) "
          f"with self<={args.self_cap}, harq<={args.harq_cap}")

    # ---- 3. report per bin, in section 10.2's shape -------------------------------------------------
    print("\n  modulation  width   " + "".join(f"{s:>15s}" for _, _, s in SNR_EDGES))
    for qm in sorted({k[0] for k in drawn}):
        for _, wname in WIDTH_EDGES + ((10 ** 9, ">24"),):
            cells = []
            any_cell = False
            for _, _, sname in SNR_EDGES:
                s = len(drawn.get((qm, wname, sname, False), ()))
                h = len(drawn.get((qm, wname, sname, True), ()))
                cells.append(f"{s:7d}/{h:<7d}" if (s or h) else f"{'—':>15s}")
                any_cell = any_cell or s or h
            if any_cell:
                print(f"  {QM_NAME.get(qm, qm):<11s} {wname:<7s} " + "".join(cells))

    if args.dry_run:
        print("\ndry run: nothing written")
        return 0

    # ---- 4. write one .npy per array, memory-mappable ----------------------------------------------
    #
    # Grouped by source file and opened once per file: the mother set is a hundred and sixty gigabytes
    # and a per-array loop would reopen all two hundred and forty-seven archives fourteen times. This
    # way each file is read once and every array's contribution is taken from that one read.
    by_file = collections.defaultdict(list)
    for key in sorted(drawn):
        for fi, si in drawn[key]:
            by_file[fi].append(si)

    pieces = collections.defaultdict(list)
    have_uci = True
    for fi in sorted(by_file):
        with np.load(files[fi], allow_pickle=False) as z:
            rows = np.asarray(sorted(by_file[fi]), dtype=np.int64)
            for name in ARRAYS:
                if name not in z.files:
                    print(f"FATAL: required array {name} missing from {os.path.basename(files[fi])}",
                          file=sys.stderr)
                    return 1
                pieces[name].append(z[name][rows])
            if have_uci:
                if OPTIONAL[0] in z.files:
                    for name in OPTIONAL:
                        pieces[name].append(z[name][rows])
                else:
                    have_uci = False

    os.makedirs(args.dst, exist_ok=True)
    written = 0
    for name, chunks in pieces.items():
        # RAGGED ARRAYS ARE PADDED, NOT REFUSED. `bits` and `tb_bits` are packed per sample and their
        # second dimension is the widest in the file they came from -- 3 672 bits in one leg, 3 456 in
        # another, and 1 985 against 2 370 for the blocks. Concatenating files of different widths has
        # to pad, and padding is safe because every consumer reads `nof_bits` / `nof_tb_bits` and never
        # looks past it. A file that stored them unpadded would fail here, which is the right outcome:
        # silent truncation is the alternative and it corrupts samples.
        if len({c.shape[1:] for c in chunks}) > 1:
            width = max(c.shape[1] for c in chunks)
            padded = []
            for c in chunks:
                if c.shape[1] == width:
                    padded.append(c)
                else:
                    pad = np.zeros((c.shape[0], width - c.shape[1]) + c.shape[2:], dtype=c.dtype)
                    padded.append(np.concatenate([c, pad], axis=1))
            chunks = padded
            print(f"  note: {name} padded to width {width} across files of differing widths")
        stacked = np.concatenate(chunks, axis=0)
        out = os.path.join(args.dst, name + ".npy")
        np.save(out, stacked)
        written += 1
        print(f"  {name:20s} {str(stacked.shape):24s} {stacked.dtype}  "
              f"{os.path.getsize(out) / 1048576:8.1f} MB")

    print(f"\nwrote {written} array(s) for {kept} sample(s) into {args.dst}")
    print(f"train with: --train {args.dst}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
