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

WHY THE WRITE STREAMS. The full draw is a hundred and sixty-six thousand samples at about a hundred and
sixteen kilobytes each -- nineteen gigabytes. The first version held every selected chunk and then
called `np.concatenate`, which needs the chunks plus a second copy at the same time, about thirty-four
gigabytes of resident memory on a thirty-one gigabyte machine. The kernel killed it at the concatenate
after twenty-one minutes, having written nothing. The write now preallocates one `.npy` memmap per array
and fills it one source file at a time, so peak memory is one source file plus the bin index and the
dataset is limited by disk rather than by RAM. The scan, bins, seed, sorted-bin iteration, `rng.choice`
calls, by-file and sorted-row order, arrays and padding are unchanged.

WIDTH AND SNR BANDS. Width comes from `alloc_nof_prb` and uses the same edges the replay test builder
does: <=3, <=8, <=24, >24. SNR comes from `snr_nominal` with the project's edges: <0, 0-6, 6-10,
10-14, 14-20, 20-30, >=30. Same edges as section 10.2's table so the report can be read against it.

PACKED WIDTHS. `bits` and `tb_bits` are packed per sample, and their second dimension is the widest in
the file they came from -- the rebuilt set has thirty-one widths for `bits` (99 to 7 344) and forty-two
for `tb_bits` (11 to 6 787). Concatenating files of different widths has to pad. The packed arrays store
BYTES while `nof_bits` / `nof_tb_bits` count BITS, so the bound that must hold is
`ceil(nof_bits / 8) <= width`, not `nof_bits <= width`: a row may legitimately carry up to eight times
the byte width in bits. The tool asserts the byte bound after the fill, and every consumer reads
`nof_bits` / `nof_tb_bits` and never looks past it.

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
import zipfile

import numpy as np

# Kept in step with `Batches.ARRAYS` / `Batches.UCI_ARRAYS` in train_depth3.py.
ARRAYS = ("X", "H", "bits", "nof_bits", "alloc_first_prb", "alloc_nof_prb", "dmrs_bitmap",
          "qm", "sigma2_over_r2", "snr_nominal", "tb_bits", "nof_tb_bits", "rnti", "n_id",
          "rv")
OPTIONAL = ("uci_nof_harq_ack", "uci_ack_re", "uci_ack_rvd_re", "uci_csi1_re")
# Packed per sample; their second dimension differs between files.
RAGGED = ("bits", "tb_bits")

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


def npy_header(zf, member):
    """Shape and dtype of one `.npy` inside an `.npz`, without materialising the array.

    The write needs the widths before it can preallocate, and opening every archive once to read
    nineteen headers is cheap; reading the arrays to ask their shape would not be.
    """
    with zf.open(member) as f:
        version = np.lib.format.read_magic(f)
        if version == (1, 0):
            shape, fortran, dtype = np.lib.format.read_array_header_1_0(f)
        elif version == (2, 0):
            shape, fortran, dtype = np.lib.format.read_array_header_2_0(f)
        else:
            shape, fortran, dtype = np.lib.format._read_array_header(f, version)
    return shape, dtype


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="src", required=True, help="mother-set directory of .npz files")
    ap.add_argument("--out", dest="dst", required=True, help="output directory for the .npy files")
    ap.add_argument("--self-cap", type=int, default=2500)
    ap.add_argument("--harq-cap", type=int, default=250)
    ap.add_argument("--qm-budget-capped", default="",
                    help="like --qm-budget, but reproduce the w2 METHOD: one cap per bin shared by "
                         "every bin of that modulation, found by bisection so the modulation's total "
                         "lands on the budget. This keeps the flat-then-thin bin shape w2 has, instead "
                         "of following how many samples each bin happens to hold.")
    ap.add_argument("--qm-budget", default="",
                    help="per-modulation totals instead of per-bin caps, as Qm:count,... "
                         "(e.g. '2:32000,4:32000,6:32000,8:64000'). Each modulation's budget is spread "
                         "across its own bins in proportion to how many samples they hold, so the width "
                         "and SNR shape inside a modulation is preserved and only its share changes.")
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
    #
    # TWO WAYS TO DECIDE HOW MUCH EACH BIN CONTRIBUTES. The original recipe caps every bin at the same
    # number, which is right when the mix of modulations should stay as the scheduler produced it. A
    # per-modulation budget is right when the mix itself is the thing being changed: the weak
    # modulation needs a larger share, and within it the width and SNR shape should still be the one
    # the captures actually contain -- so the budget is spread across that modulation's bins in
    # proportion to bin size rather than divided equally, which would over-weight the thin bins.
    rng = np.random.default_rng(args.seed)
    drawn = {}

    def draw_bin(key, count):
        members = bins[key]
        if len(members) <= count:
            return list(members)
        pick = rng.choice(len(members), size=count, replace=False)
        return [members[j] for j in sorted(pick)]

    if args.qm_budget_capped:
        # W2'S OWN METHOD, PER MODULATION. w2 capped every bin at the same number, which makes each
        # bin contribute equally right up to where it runs out; that is the "bin composition" a
        # re-draw has to keep. Here the cap is per modulation, found by bisection so the modulation's
        # total hits its budget, and harq keeps w2's tenth-of-the-cap ratio.
        budget = {}
        for item in args.qm_budget_capped.split(","):
            qm, count = item.split(":")
            budget[int(qm)] = int(count)
        for qm in sorted(budget):
            keys = [k for k in bins if k[0] == qm]
            if not keys:
                print(f"FATAL: no samples for Qm={qm}", file=sys.stderr)
                return 1

            def total_at(cap):
                return sum(min(len(bins[k]), cap if not k[3] else max(1, cap // 10)) for k in keys)

            lo, hi = 1, max(len(bins[k]) for k in keys)
            while lo < hi:
                mid = (lo + hi) // 2
                if total_at(mid) >= budget[qm]:
                    hi = mid
                else:
                    lo = mid + 1
            cap = lo
            got = 0
            for k in keys:
                limit = cap if not k[3] else max(1, cap // 10)
                drawn[k] = draw_bin(k, min(len(bins[k]), limit))
                got += len(drawn[k])
            print(f"Qm={qm}: budget {budget[qm]}, bin cap {cap} (harq {max(1, cap // 10)}), drew {got}")
        kept = sum(len(v) for v in drawn.values())
        print(f"drew {kept} sample(s) ({kept / total * 100:.1f}% of the mother set) "
              f"with per-bin caps sized to {args.qm_budget_capped}")
        by_qm = collections.Counter()
        for key, members in drawn.items():
            by_qm[key[0]] += len(members)
        print("modulation mix: " + "  ".join(
            f"{QM_NAME.get(qm, qm)} {by_qm[qm]} ({by_qm[qm] / kept * 100:.1f}%)" for qm in sorted(by_qm)))
    elif args.qm_budget:
        budget = {}
        for item in args.qm_budget.split(","):
            qm, count = item.split(":")
            budget[int(qm)] = int(count)
        missing = set(budget) - {k[0] for k in bins}
        if missing:
            print(f"FATAL: no samples for Qm={sorted(missing)} in {args.src}", file=sys.stderr)
            return 1
        for qm in sorted(budget):
            keys = [k for k in bins if k[0] == qm]
            want = budget[qm]
            # WATER-FILL, because a thin bin cannot give more than it holds and the surplus has to go
            # somewhere: allocate proportionally, cap at bin size, and repeat with the remainder until
            # the budget is spent or every bin is exhausted.
            quota = {k: 0 for k in keys}
            remaining = want
            open_keys = list(keys)
            while remaining > 0 and open_keys:
                weight = sum(len(bins[k]) for k in open_keys)
                if weight == 0:
                    break
                allocated = 0
                for k in list(open_keys):
                    share = remaining * len(bins[k]) / weight
                    take = min(int(share), len(bins[k]) - quota[k])
                    quota[k] += take
                    allocated += take
                    if quota[k] >= len(bins[k]):
                        open_keys.remove(k)
                if allocated == 0:
                    # rounding left the budget with less than one sample per open bin: give the rest to
                    # the largest bins, which is where they came from statistically.
                    for k in sorted(open_keys, key=lambda k: -len(bins[k]))[:remaining]:
                        if quota[k] < len(bins[k]):
                            quota[k] += 1
                    break
                remaining -= allocated
            got = 0
            for k in keys:
                if quota[k]:
                    drawn[k] = draw_bin(k, quota[k])
                    got += len(drawn[k])
            print(f"Qm={qm}: budget {want}, drew {got} across {sum(1 for k in keys if quota[k])} bin(s)")
        kept = sum(len(v) for v in drawn.values())
        print(f"drew {kept} sample(s) ({kept / total * 100:.1f}% of the mother set) "
              f"with a per-modulation budget of {args.qm_budget}")
        by_qm = collections.Counter()
        for key, members in drawn.items():
            by_qm[key[0]] += len(members)
        print("modulation mix: " + "  ".join(
            f"{QM_NAME.get(qm, qm)} {by_qm[qm]} ({by_qm[qm] / kept * 100:.1f}%)" for qm in sorted(by_qm)))
    else:
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

    # ---- 4. flatten the draw into output order: file ascending, row ascending -----------------------
    by_file = collections.defaultdict(list)
    for key in sorted(drawn):
        for fi, si in drawn[key]:
            by_file[fi].append(si)
    for fi in by_file:
        by_file[fi] = np.asarray(sorted(by_file[fi]), dtype=np.int64)
    order = sorted(by_file)
    assert sum(len(by_file[fi]) for fi in order) == kept
    del bins, drawn

    # ---- 5. preallocate one .npy memmap per array ---------------------------------------------------
    #
    # The shapes come from the headers of the files that will actually be read, so the padding width
    # is the widest selected chunk. Dtypes and non-packed shapes must agree across those files.
    tails = collections.defaultdict(set)          # array -> {tail shape}
    dtypes = collections.defaultdict(set)         # array -> {dtype}
    missing = set()
    for fi in order:
        with zipfile.ZipFile(files[fi]) as zf:
            members = set(zf.namelist())
            for name in ARRAYS + OPTIONAL:
                if name + ".npy" not in members:
                    missing.add(name)
                    continue
                shape, dtype = npy_header(zf, name + ".npy")
                tails[name].add(shape[1:])
                dtypes[name].add(np.dtype(dtype))

    for name in ARRAYS:
        if name in missing:
            print(f"FATAL: required array {name} missing from a selected file", file=sys.stderr)
            return 1
    have_uci = not any(name in missing for name in OPTIONAL)

    os.makedirs(args.dst, exist_ok=True)
    outs = {}
    shapes = {}
    for name in list(ARRAYS) + (list(OPTIONAL) if have_uci else []):
        if len(dtypes[name]) != 1:
            print(f"FATAL: {name} has mixed dtypes {dtypes[name]}", file=sys.stderr)
            return 1
        dtype = next(iter(dtypes[name]))
        widths = {t[0] for t in tails[name] if t}
        if name in RAGGED:
            width = max(widths)
            tail = (width,)
            if len(widths) > 1:
                # PADDED, NOT REFUSED. Concatenating files of different widths has to pad, and padding
                # is safe because every consumer reads `nof_bits` / `nof_tb_bits` and never looks past
                # it. A file that stored them unpadded would fail here, which is the right outcome:
                # silent truncation is the alternative and it corrupts samples.
                print(f"  note: {name} padded to width {width} across files of differing widths")
        else:
            if len(tails[name]) != 1:
                print(f"FATAL: {name} has differing shapes {tails[name]}", file=sys.stderr)
                return 1
            tail = next(iter(tails[name]))
        shapes[name] = (kept,) + tail
        outs[name] = np.lib.format.open_memmap(os.path.join(args.dst, name + ".npy"), mode="w+",
                                               dtype=dtype, shape=shapes[name])

    # ---- 6. fill one source file at a time ----------------------------------------------------------
    pos = 0
    for fi in order:
        rows = by_file[fi]
        m = len(rows)
        with np.load(files[fi], allow_pickle=False) as z:
            for name, out in outs.items():
                chunk = z[name][rows]
                if name in RAGGED and chunk.ndim == 2:
                    out[pos:pos + m, :chunk.shape[1]] = chunk
                else:
                    out[pos:pos + m] = chunk
        pos += m
    assert pos == kept

    # The packed arrays count BITS per sample while storing BYTES, so the bound that must hold is
    # ceil(nof_bits / 8) <= width, not nof_bits <= width. 14 634 rows of the real draw have bit counts
    # that are not multiples of eight, which is legal; a literal nof_bits <= width test would reject
    # every one of them. Check the byte bound rather than assume it.
    for packed, count in (("bits", "nof_bits"), ("tb_bits", "nof_tb_bits")):
        need = int((np.asarray(outs[count]).max() + 7) // 8)
        width = outs[packed].shape[1]
        print(f"  check: max ceil({count}/8) = {need} <= {packed} width {width} bytes")
        if need > width:
            print(f"FATAL: {count} needs {need} bytes but {packed} is {width}", file=sys.stderr)
            return 1

    for out in outs.values():
        out.flush()

    written = 0
    for name, out in outs.items():
        b = os.path.getsize(out.filename)
        written += 1
        print(f"  {name:20s} {str(shapes[name]):24s} {out.dtype}  {b / 1048576:8.1f} MB")

    print(f"\nwrote {written} array(s) for {kept} sample(s) into {args.dst}")
    print(f"train with: --train {args.dst}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
