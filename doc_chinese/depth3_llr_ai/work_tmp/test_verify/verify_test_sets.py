#!/usr/bin/env python3
"""INDEPENDENT verification of the w2 test-set draw (simple random, no --exclude).

Nothing is imported from sample_random.py: the unit sets, the key/dtype contract, the composition, the 112-bin
coverage, exact chance baselines and a from-scratch replay of the documented draw are all re-derived here.

Semantics requested by the parent:
  * train INTERSECT test1 / test2 are EXPECTED and are reported as numbers plus a per-bin breakdown.
  * test1 INTERSECT test2 is still required to be 0 -- but note the tool deduplicates ROWS, not capture names, and
    the mother set contains repeated capture names, so the unit-level intersection is reported exactly as observed.
Read-only: the mother set and the training set are only ever opened.
"""
import collections
import json
import os
from math import comb
import sys
import zipfile

import numpy as np

MOTHER = "/home/jwang/work/ai_phy_train/data/datasets_full_w2"
TRAIN = "/home/jwang/work/ai_phy_train/data/datasets_train"
TEST = "/home/jwang/work/ai_phy_train/data/datasets_test"
SEED = 20261031
COUNT = 10000
PREFIX = "w2_test"

MODS = {2: "QPSK", 4: "16QAM", 6: "64QAM", 8: "256QAM"}
MOD_ORDER = ["QPSK", "16QAM", "64QAM", "256QAM"]
WIDTH_EDGES = [3, 8, 24]
WIDTH_ORDER = ["<=3", "<=8", "<=24", ">24"]
SNR_EDGES = [0.0, 6.0, 10.0, 14.0, 20.0, 30.0]
SNR_ORDER = ["<0", "0-6", "6-10", "10-14", "14-20", "20-30", ">=30"]
POP_WORDS = {"self": "self", "harq": "harq", "harq_only": "harq"}


def width_band(prb):
    for edge in WIDTH_EDGES:
        if prb <= edge:
            return "<={}".format(edge)
    return ">{}".format(WIDTH_EDGES[-1])


def snr_band(snr):
    prev = None
    for edge in SNR_EDGES:
        if snr < edge:
            return "<{}".format(int(edge)) if edge == SNR_EDGES[0] else "{}-{}".format(int(prev), int(edge))
        prev = edge
    return ">={}".format(int(SNR_EDGES[-1]))


def pop_of(base):
    for w in ("harq_only", "harq", "self"):
        if base.endswith("_" + w + ".npz"):
            return POP_WORDS[w]
    return None


def bins_of(qm, prb, snr):
    out = []
    for q, p, s in zip(qm, prb, snr):
        m = MODS.get(int(q))
        out.append(None if m is None else (m, width_band(int(p)), snr_band(float(s))))
    return out


def npz_header(path):
    out = {}
    with zipfile.ZipFile(path) as z:
        for name in z.namelist():
            if not name.endswith(".npy"):
                continue
            with z.open(name) as fh:
                ver = np.lib.format.read_magic(fh)
                if ver == (1, 0):
                    shape, _f, dtype = np.lib.format.read_array_header_1_0(fh)
                elif ver == (2, 0):
                    shape, _f, dtype = np.lib.format.read_array_header_2_0(fh)
                else:
                    raise SystemExit("unsupported npy version {} in {}".format(ver, path))
            out[name[:-4]] = (dtype, tuple(shape))
    return out


def mult_hist(counter):
    return collections.Counter(counter.values())


def expected_overlap_uniform(mh, rows_a, rows_b, total):
    """Chance |A INTERSECT B| in units if both were independent UNIFORM row samples."""
    pa, pb = rows_a / total, rows_b / total
    return sum(cnt * (1 - (1 - pa) ** m) * (1 - (1 - pb) ** m) for m, cnt in mh.items())


def expected_overlap_partition(mh, per_side, total):
    """Chance |A INTERSECT B| when the two sets are a random PARTITION of one 2*per_side row draw.

    This is the right baseline for test1/test2: the tool draws 20,000 rows without replacement and splits them, so a
    capture owning a single row can NEVER fall in both halves. Treating the halves as independent draws (the formula
    above) over-counts those single-row captures and is therefore not used for this pair.
    """
    n = 2 * per_side
    p = n / total
    e = 0.0
    for m, cnt in mh.items():
        s = 0.0
        for k in range(2, m + 1):
            pk = comb(m, k) * p ** k * (1 - p) ** (m - k)
            s += pk * (1 - 2 * comb(per_side, k) / comb(n, k))
        e += cnt * s
    return e


def pct(n, d):
    return "{:6.2f}%".format(100.0 * n / d) if d else "   n/a"


def main():
    hard = []
    print("=" * 104)
    print("SECTION A -- the two delivered test files")
    print("=" * 104)
    paths = [os.path.join(TEST, "{}{}.npz".format(PREFIX, i)) for i in (1, 2)]
    for p in paths:
        if not os.path.exists(p):
            print("MISSING: {}".format(p))
            return 2
    sizes = {}
    for p in paths:
        sizes[p] = os.path.getsize(p)
        print("  {:<14} {:>15,} bytes  ({:.3f} GB)".format(os.path.basename(p), sizes[p], sizes[p] / 1e9))
    d = {}
    for p in paths:
        z = np.load(p, allow_pickle=True)
        d[p] = {k: z[k] for k in z.files}
        z.close()
    for p in paths:
        n = int(d[p]["qm"].shape[0])
        print("  {:<14} samples={:,}  keys={}  {}".format(
            os.path.basename(p), n, len(d[p]), "OK" if n == COUNT else "MISMATCH"))
        if n != COUNT:
            hard.append("{} has {:,} samples, expected {:,}".format(os.path.basename(p), n, COUNT))

    # ---- one full pass over the mother set + the training set, units only -----------------------------------
    mother_files = sorted(f for f in os.listdir(MOTHER) if f.endswith(".npz"))
    units_by_file, counts = {}, []
    mother_units = collections.Counter()
    for fi, f in enumerate(mother_files):
        z = np.load(os.path.join(MOTHER, f), allow_pickle=True)
        u = [str(x) for x in z["unit"]]
        z.close()
        units_by_file[fi] = u
        counts.append(len(u))
        mother_units.update(u)
    mother_rows = sum(counts)
    n_distinct = len(mother_units)

    train_files = sorted(f for f in os.listdir(TRAIN) if f.endswith(".npz"))
    train_units, per_train = collections.Counter(), {}
    for f in train_files:
        z = np.load(os.path.join(TRAIN, f), allow_pickle=True)
        u = [str(x) for x in z["unit"]]
        z.close()
        per_train[f] = (len(u), len(set(u)))
        train_units.update(u)
    train_rows = sum(v[0] for v in per_train.values())
    train_set = set(train_units)

    t1 = [str(x) for x in d[paths[0]]["unit"]]
    t2 = [str(x) for x in d[paths[1]]["unit"]]
    t1_set, t2_set = set(t1), set(t2)
    inter_t1, inter_t2, inter_12 = train_set & t1_set, train_set & t2_set, t1_set & t2_set

    print("\n" + "=" * 104)
    print("SECTION B -- unit identity sets and the three intersections")
    print("=" * 104)
    print("  mother set: {:,} rows across {} npz; {:,} DISTINCT unit names; {:,} captures repeat "
          "({:,} extra rows), max multiplicity {}.".format(
              mother_rows, len(mother_files), n_distinct, mother_rows - n_distinct,
              mother_rows - n_distinct, max(mother_units.values())))
    print("  -> 'unit' is NOT unique per row, so a row-level draw can put the same capture in two sets.\n")
    print("  training files ({}):".format(len(train_files)))
    for f in train_files:
        r, uq = per_train[f]
        print("    {:<32} {:>9,} rows   {:>9,} distinct units".format(f, r, uq))
    print("    {:<32} {:>9,} rows   {:>9,} distinct units".format("TOTAL", train_rows, len(train_set)))

    print("\n  |train| = {:,} distinct units   ({:,} rows)".format(len(train_set), train_rows))
    print("  |test1| = {:,} distinct units   ({:,} rows)".format(len(t1_set), len(t1)))
    print("  |test2| = {:,} distinct units   ({:,} rows)".format(len(t2_set), len(t2)))
    print("\n  |train INTERSECT test1| = {:,} units   (per-row overlap {:,} / {:,} = {})".format(
        len(inter_t1), sum(1 for u in t1 if u in train_set), len(t1),
        pct(sum(1 for u in t1 if u in train_set), len(t1))))
    print("  |train INTERSECT test2| = {:,} units   (per-row overlap {:,} / {:,} = {})".format(
        len(inter_t2), sum(1 for u in t2 if u in train_set), len(t2),
        pct(sum(1 for u in t2 if u in train_set), len(t2))))
    print("  |test1 INTERSECT test2| = {:,} units".format(len(inter_12)))

    mh = mult_hist(mother_units)
    exp_t1 = expected_overlap_uniform(mh, train_rows, COUNT, mother_rows)
    exp_12 = expected_overlap_partition(mh, COUNT, mother_rows)
    exp_12_wrong = expected_overlap_uniform(mh, COUNT, COUNT, mother_rows)
    print("\n  chance baselines (repeated capture names accounted for via their multiplicity in the mother set):")
    print("    train vs test: E = {:.1f} per set if the {:,}-row training draw were UNIFORM and independent".format(
        exp_t1, train_rows))
    print("      (it is not uniform -- it is stratified by bin -- so this is a reference point, not an exact null)")
    print("      observed test1 {:,} = {:.2f}x   test2 {:,} = {:.2f}x".format(
        len(inter_t1), len(inter_t1) / exp_t1, len(inter_t2), len(inter_t2) / exp_t1))
    print("    test1 vs test2: E = {:.1f} because the two sets are a PARTITION of one 20,000-row draw".format(exp_12))
    print("      (the naive independent-draw formula would wrongly say {:.1f}, since it lets a 1-row capture,".format(
        exp_12_wrong))
    print("       which is {:,} of {:,} names, land in both halves)    observed {:,} = {:.2f}x".format(
        mh.get(1, 0), n_distinct, len(inter_12), len(inter_12) / exp_12 if exp_12 else float("nan")))

    if inter_12:
        hard.append("test1 INTERSECT test2 = {} units (non-zero)".format(len(inter_12)))
        print("\n  !! test1 INTERSECT test2 is NON-ZERO. All {} offending unit(s), with their multiplicity in the".format(
            len(inter_12)))
        print("     mother set and how many rows each set holds. The reason is structural: the tool draws 20,000")
        print("     distinct ROWS, and a capture name that owns several rows can land in both sets.")
        print("     {:<44} {:>4} {:>6} {:>6}".format("unit", "moth", "test1", "test2"))
        c1 = collections.Counter(t1)
        c2 = collections.Counter(t2)
        for u in sorted(inter_12):
            print("     {:<44} {:>4} {:>6} {:>6}".format(u, mother_units[u], c1[u], c2[u]))
    else:
        print("\n  -> test1 and test2 are disjoint at unit level (hard requirement met)")
    print("\n  duplicate units WITHIN test1: {} ({} extra rows); WITHIN test2: {} ({} extra rows)".format(
        sum(1 for c in collections.Counter(t1).values() if c > 1), len(t1) - len(t1_set),
        sum(1 for c in collections.Counter(t2).values() if c > 1), len(t2) - len(t2_set)))

    print("\n" + "=" * 104)
    print("SECTION C -- key and dtype contract against the mother set")
    print("=" * 104)
    mother_keys, mother_dtype, widths = set(), collections.defaultdict(set), collections.defaultdict(set)
    sigs = collections.Counter()
    for f in mother_files:
        h = npz_header(os.path.join(MOTHER, f))
        sigs[tuple(sorted(h))] += 1
        for k, (dt, shp) in h.items():
            mother_keys.add(k)
            mother_dtype[k].add(str(dt))
            if k in ("bits", "tb_bits") and len(shp) == 2:
                widths[k].add(shp[1])
    print("  mother files={}  distinct key-signatures among them={}  union of keys={}".format(
        len(mother_files), len(sigs), len(mother_keys)))
    for sig, c in sigs.items():
        print("    {} file(s) with {} keys".format(c, len(sig)))
    print("    keys: {}".format(", ".join(sorted(mother_keys))))
    test_h = {p: npz_header(p) for p in paths}
    print("\n  {:<16} {:<14} {:<14} {:<14}".format("key", "mother dtype", "test1 dtype", "test2 dtype"))
    for k in sorted(mother_keys):
        md = "/".join(sorted(mother_dtype[k]))
        cells, bad = [], False
        for p in paths:
            if k not in test_h[p]:
                cells.append("MISSING")
                bad = True
                hard.append("key {!r} missing from {}".format(k, os.path.basename(p)))
            else:
                cells.append(str(test_h[p][k][0]))
                if str(test_h[p][k][0]) not in mother_dtype[k]:
                    bad = True
                    hard.append("dtype mismatch {!r} in {}".format(k, os.path.basename(p)))
        print("  {:<16} {:<14} {:<14} {:<14} {}".format(k, md, cells[0], cells[1], "MISMATCH" if bad else "ok"))
    extra = [sorted(set(test_h[paths[0]]) - mother_keys), sorted(set(test_h[paths[1]]) - mother_keys)]
    print("\n  test-only keys: test1={} test2={}".format(extra[0] or "none", extra[1] or "none"))
    if extra[0] or extra[1]:
        hard.append("test files carry keys absent from the mother set")
    for k in ("bits", "tb_bits"):
        print("  {:<9} mother widths {} -> test1 {} test2 {} (zero-pad to common width = expected)".format(
            k, sorted(widths[k]), test_h[paths[0]][k][1][1], test_h[paths[1]][k][1][1]))

    print("\n" + "=" * 104)
    print("SECTION D -- array sanity (load, non-empty, aligned row counts)")
    print("=" * 104)
    for p in paths:
        x = d[p]
        empt = [k for k in x if x[k].size == 0]
        n = int(x["X"].shape[0])
        lens = {k: int(x[k].shape[0]) for k in ("alloc_nof_prb", "snr_nominal", "qm", "unit")}
        print("  {:<14} X.shape={} | rows: X={:,} alloc_nof_prb={:,} snr_nominal={:,} qm={:,} unit={:,}".format(
            os.path.basename(p), x["X"].shape, n, lens["alloc_nof_prb"], lens["snr_nominal"], lens["qm"], lens["unit"]))
        print("                 empty arrays: {} | leading-dim mismatches: {}".format(
            empt or "none", {k: int(x[k].shape[0]) for k in x if x[k].ndim and int(x[k].shape[0]) != n} or "none"))
        print("                 X dtype={} range=[{:.4g}, {:.4g}] all-finite={} | unit[0]={!r}".format(
            x["X"].dtype, float(x["X"].min()), float(x["X"].max()), bool(np.isfinite(x["X"]).all()),
            str(x["unit"][0])))
        if empt:
            hard.append("empty array(s) in {}: {}".format(os.path.basename(p), empt))
        if not (n == lens["alloc_nof_prb"] == lens["snr_nominal"] == COUNT):
            hard.append("row-count contract broken in {}".format(os.path.basename(p)))

    print("\n" + "=" * 104)
    print("SECTION E -- composition and provenance (replay of the documented draw)")
    print("=" * 104)
    file_of = np.repeat(np.arange(len(mother_files)), counts)
    row_of = np.concatenate([np.arange(n, dtype=np.int64) for n in counts])
    rng = np.random.default_rng(SEED)
    pick = rng.choice(mother_rows, size=2 * COUNT, replace=False)
    print("  replayed: rng=default_rng({}), choice({:,}, size={:,}, replace=False)  [no exclusion]".format(
        SEED, mother_rows, pick.size))
    prov, replay_ok = {}, True
    for slot, s in ((1, slice(0, COUNT)), (2, slice(COUNT, 2 * COUNT))):
        # the tool writes each set grouped by source file (sorted by name) and by ascending row inside a file, so the
        # stored order is NOT the order of `pick`; reproduce that reordering before comparing.
        per_sample = sorted((int(file_of[p]), int(row_of[p])) for p in pick[s])
        exp = [units_by_file[fi][ri] for fi, ri in per_sample]
        got = t1 if slot == 1 else t2
        same = exp == got
        replay_ok &= same
        print("  replay reproduces test{} exactly (units and stored order): {}".format(slot, same))
        if not same:
            print("    differing positions: {:,} of {:,}".format(sum(1 for a, b in zip(exp, got) if a != b), COUNT))
        prov[slot] = [(mother_files[fi], ri) for fi, ri in per_sample]
    if not replay_ok:
        print("  NOTE: replay mismatch; populations below still come from the replay as a best effort.")

    print("\n  --- modulation composition (qm column) ---")
    for slot, p in ((1, paths[0]), (2, paths[1])):
        c = collections.Counter(MODS.get(int(x), "qm={}".format(int(x))) for x in d[p]["qm"])
        print("  test{}: {}".format(slot, "  ".join("{} {:,} ({})".format(
            m, c.get(m, 0), pct(c.get(m, 0), COUNT)) for m in MOD_ORDER)))
        other = {k: v for k, v in c.items() if k not in MOD_ORDER}
        if other:
            print("          unmapped qm values: {}".format(other))

    print("\n  --- population composition and source files (from the mother file each row came from) ---")
    for slot, p in ((1, paths[0]), (2, paths[1])):
        cpop = collections.Counter(pop_of(os.path.basename(f)) for f, _ in prov[slot])
        cfile = collections.Counter(os.path.basename(f) for f, _ in prov[slot])
        ct = collections.Counter((MODS.get(int(q)), pop_of(os.path.basename(f)))
                                 for q, (f, _) in zip(d[p]["qm"], prov[slot]))
        print("  test{}:".format(slot))
        for popn in ("self", "harq"):
            print("    population {:<5} {:>7,}  {}".format(popn, cpop.get(popn, 0), pct(cpop.get(popn, 0), COUNT)))
        print("    distinct source files: {} / {}".format(len(cfile), len(mother_files)))
        print("    modulation x population: {}".format("  ".join(
            "{} {} {:,}".format(m, popn, ct[(m, popn)])
            for m in MOD_ORDER for popn in ("self", "harq") if ct[(m, popn)])))
        print("    top source files: {}".format(", ".join(
            "{} {:,}".format(f.replace(".npz", ""), n) for f, n in cfile.most_common(3))))

    print("\n" + "=" * 104)
    print("SECTION F -- the 112 bins (modulation x width band x SNR band)")
    print("=" * 104)
    all_bins = [(m, w, s) for m in MOD_ORDER for w in WIDTH_ORDER for s in SNR_ORDER]
    avail = collections.Counter()
    for f in mother_files:
        z = np.load(os.path.join(MOTHER, f), allow_pickle=True)
        qm, prb, snr = z["qm"][:], z["alloc_nof_prb"][:], z["snr_nominal"][:]
        z.close()
        for b in bins_of(qm, prb, snr):
            if b is not None:
                avail[b] += 1
    tr_bin = collections.Counter()
    for f in train_files:
        z = np.load(os.path.join(TRAIN, f), allow_pickle=True)
        qm, prb, snr = z["qm"][:], z["alloc_nof_prb"][:], z["snr_nominal"][:]
        z.close()
        for b in bins_of(qm, prb, snr):
            tr_bin[b] += 1
    te, ov, ind = {}, {}, {}
    for slot, p, tl in ((1, paths[0], t1), (2, paths[1], t2)):
        cb, co, ci = collections.Counter(), collections.Counter(), collections.Counter()
        for u, b in zip(tl, bins_of(d[p]["qm"], d[p]["alloc_nof_prb"], d[p]["snr_nominal"])):
            if b is None:
                continue
            cb[b] += 1
            (co if u in train_set else ci)[b] += 1
        te[slot], ov[slot], ind[slot] = cb, co, ci
    full_bins = {b for b in all_bins if avail.get(b) and tr_bin.get(b, 0) == avail[b]}

    print("  {:<8} {:<6} {:<8} {:>10} {:>8} {:>7} {:>7} {:>6} {:>6} {:>6} {:>6}  {}".format(
        "mod", "width", "snr", "available", "train", "test1", "test2", "ov1", "ind1", "ov2", "ind2", "train/avail"))
    for b in all_bins:
        a, t = avail.get(b, 0), tr_bin.get(b, 0)
        print("  {:<8} {:<6} {:<8} {:>10,} {:>8,} {:>7,} {:>7,} {:>6,} {:>6,} {:>6,} {:>6,} {:>5.1f}%{}".format(
            b[0], b[1], b[2], a, t, te[1].get(b, 0), te[2].get(b, 0), ov[1].get(b, 0), ind[1].get(b, 0),
            ov[2].get(b, 0), ind[2].get(b, 0), 100.0 * t / a if a else 0.0,
            "  TRAIN TOOK ALL" if a and t == a else ""))
    print("  ov  = test rows whose unit is also in the training set (a training duplicate)")
    print("  ind = test rows whose unit is not in the training set (the independent coverage)")

    print("\n  where the overlap sits: bins ranked by overlap share ov1/test1 (min 10 test samples)")
    print("    {:<8} {:<6} {:<8} {:>10} {:>8} {:>7} {:>6} {:>7}  {}".format(
        "mod", "width", "snr", "available", "train", "test1", "ov1", "share", "train/avail"))
    ranked = sorted((b for b in all_bins if te[1].get(b, 0) >= 10),
                    key=lambda b: -ov[1].get(b, 0) / te[1][b])[:12]
    for b in ranked:
        print("    {:<8} {:<6} {:<8} {:>10,} {:>8,} {:>7,} {:>6,} {:>6.1f}%  {:>5.1f}%".format(
            b[0], b[1], b[2], avail.get(b, 0), tr_bin.get(b, 0), te[1].get(b, 0), ov[1].get(b, 0),
            100.0 * ov[1].get(b, 0) / te[1][b], 100.0 * tr_bin.get(b, 0) / max(avail.get(b, 0), 1)))
    print("  (the two right-hand columns track each other: a test sample in a bin that training filled to X% is a")
    print("   training duplicate with probability about X%, which is why the overlap is large in absolute terms but")
    print("   is not enriched over chance overall.)")

    print("\n  bins populated in the mother set: {} / 112".format(sum(1 for b in all_bins if avail.get(b))))
    for slot in (1, 2):
        zero = [b for b in all_bins if te[slot].get(b, 0) == 0]
        zp = [b for b in zero if avail.get(b, 0) > 0]
        zind = [b for b in all_bins if te[slot].get(b, 0) > 0 and ind[slot].get(b, 0) == 0]
        nind, nov = sum(ind[slot].values()), sum(ov[slot].values())
        print("\n  test{}: bins with ZERO samples: {} / 112 ({} of them populated in the mother set)".format(
            slot, len(zero), len(zp)))
        for b in zp:
            print("    {:<8} {:<6} {:<8} available={:>9,} train={:>8,} ({:>5.1f}% of bin){}".format(
                b[0], b[1], b[2], avail[b], tr_bin.get(b, 0), 100.0 * tr_bin.get(b, 0) / avail[b],
                "  <-- training absorbed the whole bin" if tr_bin.get(b, 0) == avail[b] else ""))
        print("    bins with 1-9 samples: {}".format(sum(1 for b in all_bins if 0 < te[slot].get(b, 0) < 10)))
        print("    independent coverage: {:,} of {:,} rows ({}) are NOT training duplicates; {:,} rows are".format(
            nind, COUNT, pct(nind, COUNT), nov))
        print("    bins with ZERO independent coverage (bin present, but every sample is a training duplicate): {}".format(
            len(zind)))
        if zind:
            print("      {}".format(", ".join("{}/{}/{}".format(*b) for b in zind)))
        print("    overlap rows in bins training absorbed completely: {:,} of {:,}".format(
            sum(n for b, n in ov[slot].items() if b in full_bins), nov))
    print("\n  training set: bins where train rows == available rows (training took the entire bin): {} / 112".format(
        sum(1 for b in all_bins if avail.get(b) and tr_bin.get(b, 0) == avail[b])))

    print("\n" + "=" * 104)
    print("SECTION G -- disk")
    print("=" * 104)
    tot = 0
    for f in sorted(os.listdir(TEST)):
        st = os.stat(os.path.join(TEST, f))
        tot += st.st_size
        print("  {:<26} {:>15,} bytes".format(f, st.st_size))
    print("  datasets_test total: {:,} bytes ({:.3f} GB)".format(tot, tot / 1e9))
    sv = os.statvfs(TEST)
    print("  filesystem: total {:.1f} GB, free {:.1f} GB, used {:.1f}%".format(
        sv.f_blocks * sv.f_frsize / 1e9, sv.f_bavail * sv.f_frsize / 1e9,
        100.0 * (sv.f_blocks - sv.f_bfree) / sv.f_blocks))

    print("\n" + "=" * 104)
    print("VERDICT")
    print("=" * 104)
    print("  reported, not fixed (by instruction):")
    print("    |train INTERSECT test1| = {:,} units, |train INTERSECT test2| = {:,} units".format(
        len(inter_t1), len(inter_t2)))
    print("    |test1 INTERSECT test2| = {:,} units".format(len(inter_12)))
    if hard:
        print("\n  FAILURES / NON-ZERO INTERSECTIONS ({}):".format(len(hard)))
        for x in hard:
            print("    - {}".format(x))
        return 1
    print("\n  ALL STRUCTURAL CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
