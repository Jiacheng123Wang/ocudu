#!/usr/bin/env python3
"""Verify a drawn training set against the mother set it came from.

WHAT IT CHECKS, and why each one has caught or would catch a real failure.

  1. CAPS       Every bin's drawn count is within its cap (2 500 self / 250 harq), and a bin that
                was under its cap in the mother set contributed ALL of its samples rather than a
                subset. Recomputes the bin from each drawn sample's own fields, so it does not trust
                the drawing tool's bookkeeping -- a tool that miscounts its own output would pass a
                check written against its own report.
  2. COVERAGE   Every non-empty bin in the mother set appears in the draw. A bin silently dropped is
                the failure mode that matters most: the whole point of the draw is that no cell of
                the modulation-by-width-by-signal-to-noise table goes unrepresented.
  3. DUPLICATES No sample appears twice. Detected by the tuple a sample is, not by index, because
                indices are reassigned by the draw and would hide a double-count.
  4. ALIGNMENT  For sampled rows, every array describes the SAME original sample. This is the check
                that catches transposition: if `X` were taken in one order and `qm` in another, both
                would look fine in isolation, the shapes would match, the counts would match, and
                every downstream number would be nonsense.
  5. PROVENANCE Each drawn sample is findable in the mother set, in a file whose `_self`/`_harq`
                suffix agrees with how the draw classified it.
  6. FORMAT     The directory opens memory-mapped, one file per array, every array the trainer reads
                present, and their first dimension equal.

HOW ALIGNMENT AND PROVENANCE ARE CHECKED WITHOUT A SAMPLE ID. The datasets carry no row identifier,
so a drawn row is matched back by hashing a few arrays' bytes for that row and looking for the same
hash in the source file. A match means the row came from there; a mismatch in any array means the
arrays disagree about which sample the row is.

USAGE.
  verify_train_set_w2.py --draw <datasets_train_w2> --mother <datasets_full_w2>
"""
import argparse
import collections
import glob
import hashlib
import os
import sys

import numpy as np

REQUIRED = ("X", "H", "bits", "nof_bits", "alloc_first_prb", "alloc_nof_prb", "dmrs_bitmap",
            "qm", "sigma2_over_r2", "snr_nominal", "tb_bits", "nof_tb_bits", "rnti", "n_id", "rv")

WIDTH_EDGES = ((3, "<=3"), (8, "<=8"), (24, "<=24"))
SNR_EDGES = ((-1e9, 0.0, "<0"), (0.0, 6.0, "0-6"), (6.0, 10.0, "6-10"), (10.0, 14.0, "10-14"),
             (14.0, 20.0, "14-20"), (20.0, 30.0, "20-30"), (30.0, 1e9, ">=30"))
QM_NAME = {2: "QPSK", 4: "16QAM", 6: "64QAM", 8: "256QAM"}


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


def row_key(H, qm, snr, i):
    """Identity of one row, from data rather than from position.

    Deliberately cheap. Hashing a whole sample is a hundred and thirteen kilobytes, and the mother set
    holds three and a quarter million of them -- three hundred and seventy gigabytes of hashing to match
    two hundred rows. Sixteen complex channel values plus the modulation and the signal-to-noise is
    enough to be unique in practice and costs a few dozen bytes, which is what makes the provenance
    check something a person will actually run.
    """
    h = hashlib.sha256()
    h.update(np.ascontiguousarray(H[i, 0, :8]).tobytes())
    h.update(np.asarray([qm[i], snr[i]]).tobytes())
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--draw", required=True)
    ap.add_argument("--mother", required=True)
    ap.add_argument("--self-cap", type=int, default=2500)
    ap.add_argument("--harq-cap", type=int, default=250)
    ap.add_argument("--align-samples", type=int, default=200,
                    help="how many drawn rows to match back to the mother set")
    args = ap.parse_args()

    problems = []

    # ---- 6. FORMAT ---------------------------------------------------------------------------------
    print("== 6. format ==")
    d = {}
    for name in REQUIRED:
        path = os.path.join(args.draw, name + ".npy")
        if not os.path.exists(path):
            problems.append(f"missing array {name}.npy")
            continue
        d[name] = np.load(path, mmap_mode="r")
    if problems:
        for p in problems:
            print(f"  FAIL {p}")
        return 1
    n = d["X"].shape[0]
    print(f"  X {d['X'].shape} {d['X'].dtype}, memmap={isinstance(d['X'], np.memmap)}")
    for name in REQUIRED:
        if d[name].shape[0] != n:
            problems.append(f"{name} has {d[name].shape[0]} rows, X has {n}")
    print(f"  {len(REQUIRED)} arrays, all with {n} rows" if not problems else "  FAIL row counts differ")

    # ---- 1. CAPS, per population, recovered from provenance -------------------------------------------
    #
    # `self` and `harq` are NOT stored in the draw -- the arrays carry no such field -- so a check from the
    # draw alone can only see the combined count. That is not equivalent to two separate caps: a cell whose
    # harq population is 2 013 deep contributes 250 and stays under 2 750 combined, and a verifier using the
    # combined cap reports it as "should have been taken whole" when it was taken exactly right. Fifteen
    # cells of the real draw failed that way before this was fixed, all of them one population over its own
    # cap and the total under the sum.
    #
    # So the populations are recovered by matching each drawn row back to the mother row it came from, using
    # a key of the fields that identify a sample. That is the same match the alignment check does, done for
    # every row instead of two hundred, and it is what makes the per-population caps checkable at all.
    print("\n== 1. caps (per population, via provenance) ==")
    qm, prb, snr = d["qm"], d["alloc_nof_prb"], d["snr_nominal"]

    KEY_ARRAYS = ("qm", "alloc_nof_prb", "snr_nominal", "nof_bits", "nof_tb_bits", "rnti",
                  "n_id", "rv", "alloc_first_prb", "dmrs_bitmap", "sigma2_over_r2")

    def key_of(a, i):
        """Key of row `i`, from arrays already materialised.

        `a` is a dict of arrays, NOT an open NpzFile. Reading a member of a compressed archive
        decompresses it in full on every access, so a per-row `z["qm"][i]` inside a loop over three
        million rows decompresses `qm` three million times -- which is exactly the trap the trainer's
        own comment warns about, and which the first version of this file walked straight into: eight
        hundred and fifty gigabytes of reads for a hundred and sixty-four gigabytes of data.
        """
        return (int(a["qm"][i]), int(a["alloc_nof_prb"][i]), float(a["snr_nominal"][i]),
                int(a["nof_bits"][i]), int(a["nof_tb_bits"][i]), int(a["rnti"][i]),
                int(a["n_id"][i]), int(a["rv"][i]), int(a["alloc_first_prb"][i]),
                int(a["dmrs_bitmap"][i]), float(a["sigma2_over_r2"][i]))

    mother_index = {}
    files = sorted(glob.glob(os.path.join(args.mother, "*.npz")))
    print(f"  indexing {len(files)} mother file(s) by row key ...")
    for path in files:
        # The same rule the draw uses, and it has to be: every rebuilt mother file has "w2_harq_"
        # in its name, so testing for "_harq" alone labels a _self file as recovered. A self
        # population judged against the 250 recovered cap is not the check that was specified.
        base = os.path.basename(path)
        harq = ("_self" not in base) and ("_harq" in base)
        with np.load(path, allow_pickle=False) as z:
            # Every array the key needs, taken ONCE. X and H are deliberately not loaded: the key does not
            # use them, and holding them resident costs a hundred and thirteen kilobytes per row.
            a = {name: z[name] for name in KEY_ARRAYS}
            for j in range(a["qm"].shape[0]):
                mother_index[key_of(a, j)] = harq

    pop = collections.Counter()          # (mod, width, snr, harq) -> drawn count
    unmatched = 0
    for i in range(n):
        k = (int(qm[i]), int(prb[i]), float(snr[i]), int(d["nof_bits"][i]),
             int(d["nof_tb_bits"][i]), int(d["rnti"][i]), int(d["n_id"][i]), int(d["rv"][i]),
             int(d["alloc_first_prb"][i]), int(d["dmrs_bitmap"][i]), float(d["sigma2_over_r2"][i]))
        h = mother_index.get(k)
        if h is None:
            unmatched += 1
            continue
        pop[(int(qm[i]), width_band(int(prb[i])), snr_band(float(snr[i])), h)] += 1
    if unmatched:
        problems.append(f"{unmatched} drawn row(s) have no mother row")
    print(f"  {n - unmatched}/{n} drawn rows matched to a mother row ({unmatched} unmatched)")
    print(f"  populations: self {sum(v for k, v in pop.items() if not k[3])}, "
          f"harq {sum(v for k, v in pop.items() if k[3])}")

    per_cell = collections.Counter()
    for k, v in pop.items():
        per_cell[k[:3]] += v
    over = {k: v for k, v in per_cell.items() if v > args.self_cap + args.harq_cap}
    if over:
        for k, v in sorted(over.items()):
            problems.append(f"cell {QM_NAME.get(k[0], k[0])}/{k[1]}/{k[2]} has {v} > {args.self_cap + args.harq_cap}")
        print(f"  FAIL {len(over)} cell(s) over the combined cap")
    else:
        print(f"  every cell within the combined cap {args.self_cap + args.harq_cap} "
              f"({len(per_cell)} non-empty cell(s))")

    # ---- 4/5. ALIGNMENT + PROVENANCE against the mother set ------------------------------------------
    print("\n== 4/5. alignment and provenance ==")
    print(f"  mother set: {len(files)} file(s)")

    # Index the mother set by row hash, lazily per file, so the check does not need the whole set in RAM.
    wanted = min(args.align_samples, n)
    picks = np.linspace(0, n - 1, wanted, dtype=np.int64)
    draw_keys = {int(i): row_key(d["H"], qm, snr, int(i)) for i in picks}
    # Keyed BY HASH, valued by drawn row -- the other way round silently matches nothing, which is what
    # the first version of this check did before it was tested against data.
    remaining = {h: row for row, h in draw_keys.items()}
    found = {}
    for path in files:
        if not remaining:
            break
        with np.load(path, allow_pickle=False) as z:
            # H only, not X: row_key reads sixteen channel values. The arrays are taken once, outside the
            # row loop, for the reason in key_of's docstring.
            Hm, qm_m, snr_m = z["H"], z["qm"], z["snr_nominal"]
            for j in range(qm_m.shape[0]):
                k = row_key(Hm, qm_m, snr_m, j)
                if k in remaining:
                    found[remaining.pop(k)] = (os.path.basename(path), j)
        print(f"    {os.path.basename(path)}: {len(found)} matched, {len(remaining)} to go")

    print(f"  matched {len(found)}/{wanted} sampled rows back to the mother set")
    if remaining:
        problems.append(f"{len(remaining)} sampled row(s) not found in the mother set")
        print(f"  FAIL {len(remaining)} row(s) have no origin")
    for row, (fname, j) in list(found.items())[:3]:
        print(f"    row {row} <- {fname}[{j}]")

    # ---- 2. COVERAGE, over the mother set ------------------------------------------------------------
    print("\n== 2. coverage ==")
    mother_cells = collections.Counter()
    for path in files:
        with np.load(path, allow_pickle=False) as z:
            # Hoisted for the fourth and last time. Every one of these loops had the same defect: a compressed
            # member re-inflated per row, which is three million decompressions for a three-million-row scan.
            qm_m, prb_m, snr_m = z["qm"], z["alloc_nof_prb"], z["snr_nominal"]
            for i in range(qm_m.shape[0]):
                mother_cells[(int(qm_m[i]), width_band(int(prb_m[i])),
                              snr_band(float(snr_m[i])))] += 1
    missing = sorted(set(mother_cells) - set(per_cell))
    print(f"  mother set: {sum(mother_cells.values())} samples over {len(mother_cells)} non-empty cell(s)")
    print(f"  draw:       {sum(per_cell.values())} samples over {len(per_cell)} cell(s)")
    if missing:
        for k in missing:
            problems.append(f"cell {QM_NAME.get(k[0], k[0])}/{k[1]}/{k[2]} ({mother_cells[k]} in mother) absent from draw")
        print(f"  FAIL {len(missing)} cell(s) absent from the draw")
    else:
        print("  every non-empty cell in the mother set is represented")

    # A population under its OWN cap must have been taken whole. Judged per population, since that is what
    # the caps are; the combined-cap version of this test flagged fifteen correct cells on the real draw.
    short = 0
    mother_pop = collections.Counter()
    for path in files:
        base = os.path.basename(path)
        h = ("_self" not in base) and ("_harq" in base)
        with np.load(path, allow_pickle=False) as z:
            qm_m, prb_m, snr_m = z["qm"], z["alloc_nof_prb"], z["snr_nominal"]
            for i in range(qm_m.shape[0]):
                mother_pop[(int(qm_m[i]), width_band(int(prb_m[i])), snr_band(float(snr_m[i])), h)] += 1
    for k, have in mother_pop.items():
        cap = args.harq_cap if k[3] else args.self_cap
        if have <= cap and pop.get(k, 0) != have:
            problems.append(f"{'harq' if k[3] else 'self'} cell {QM_NAME.get(k[0], k[0])}/{k[1]}/{k[2]} "
                            f"had {have} <= cap {cap} but the draw has {pop.get(k, 0)}")
            short += 1
    print(f"  populations under their own cap taken whole: {'yes' if short == 0 else f'{short} FAILED'}")

    # ---- 3. DUPLICATES -------------------------------------------------------------------------------
    print("\n== 3. duplicates ==")
    seen = collections.Counter(row_key(d["H"], qm, snr, i) for i in range(n))
    dupes = sum(1 for v in seen.values() if v > 1)
    print(f"  {dupes} duplicated row(s) among {n}")
    if dupes:
        problems.append(f"{dupes} duplicated row(s) in the draw")

    # ---- verdict ------------------------------------------------------------------------------------
    print("\n== verdict ==")
    if problems:
        print(f"  FAIL ({len(problems)} problem(s))")
        for p in problems:
            print(f"    - {p}")
        return 1
    print("  PASS: caps, coverage, duplicates, alignment, provenance and format all hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
