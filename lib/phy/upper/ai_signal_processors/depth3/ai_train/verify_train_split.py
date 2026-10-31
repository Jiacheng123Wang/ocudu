#!/usr/bin/env python3
"""Check a drawn subset against the mother set it came from, without trusting the draw's own manifest.

WHY IT EXISTS. A sampler that reports what it drew is checking itself. This re-derives every bin from the OUTPUT arrays
-- modulation from the qm column, width from alloc_nof_prb, SNR from snr_nominal -- recounts the mother set from
scratch, and then compares: no bin may exceed its quota, no bin may draw less than the smaller of its availability and
its quota, and the manifest's per-bin numbers must equal the recount. Written for the w2 training draw (quotas 2500 and
250) but the quotas are arguments, so it applies to any future draw.

It also checks the quieter failure: that every key the mother set carries reaches the output with the same dtype, which
is how the silently-dropped pilot_ls_ratio column was caught in the first place.
"""
"""Independent verification of the w2_train per-bin quota sampling.  READ-ONLY on both data dirs.

Nothing here trusts the sampler's own bookkeeping:
  * bins are re-derived from the OUTPUT arrays (qm -> modulation, alloc_nof_prb -> width band,
    snr_nominal -> SNR band, file name -> population), and the qm column is checked against the
    modulation encoded in the file name;
  * the mother set is recounted from scratch (only qm / alloc_nof_prb / snr_nominal are read) so
    `available` is not taken on trust either;
  * key/dtype presence is checked from the .npy headers of every input file (no bulk reads).

Prints a human-readable report and a PASS/FAIL summary.
"""
import collections
import json
import os
import re
import sys
import zipfile

import numpy as np

MOTHER = "/home/jwang/work/ai_phy_train/data/datasets_full_w2"
OUTDIR = "/home/jwang/work/ai_phy_train/data/datasets_train"
PREFIX = "w2_train"

MOD_BY_QM = {2: "QPSK", 4: "16QAM", 6: "64QAM", 8: "256QAM"}
WIDTH_EDGES = [3, 8, 24]
SNR_EDGES = [0.0, 6.0, 10.0, 14.0, 20.0, 30.0]
QUOTA = {"self": 2500, "harq": 250}

WLAB = ["<=3", "<=8", "<=24", ">24"]
SLAB = ["<0", "0-6", "6-10", "10-14", "14-20", "20-30", ">=30"]
MODS = ["QPSK", "16QAM", "64QAM", "256QAM"]

FAILS = []


def fail(msg):
    FAILS.append(msg)
    print("  ** FAIL: " + msg)


def note(msg):
    print("  " + msg)


# ---------------------------------------------------------------------------------------------
# Bin re-derivation, written twice: a literal reading of the spec, and a vectorised version used
# on the arrays.  The two are checked against each other on a dense grid below.
# ---------------------------------------------------------------------------------------------
def wband_ref(prb):
    for e in WIDTH_EDGES:
        if prb <= e:
            return "<={}".format(e)
    return ">{}".format(WIDTH_EDGES[-1])


def sband_ref(snr):
    prev = None
    for e in SNR_EDGES:
        if snr < e:
            return "<{}".format(int(e)) if e == SNR_EDGES[0] else "{}-{}".format(int(prev), int(e))
        prev = e
    return ">={}".format(int(SNR_EDGES[-1]))


_WARR = np.array(WIDTH_EDGES, dtype=np.int64)
_SARR = np.array(SNR_EDGES, dtype=np.float64)


def wband_vec(prb):
    return np.searchsorted(_WARR, prb, side="left")


def sband_vec(snr):
    return np.searchsorted(_SARR, snr, side="right")


def selftest():
    print("[0] binning self-test (vectorised vs literal spec)")
    bad = 0
    for prb in list(range(0, 301)) + [-1, 4, 9, 25]:
        if WLAB[int(wband_vec(np.array([prb]))[0])] != wband_ref(prb):
            bad += 1
    grid = [round(-40.0 + 0.05 * i, 3) for i in range(1600)] + \
           [-0.001, 0.0, 5.999, 6.0, 9.999, 10.0, 13.999, 14.0, 19.999, 20.0, 29.999, 30.0, 30.001]
    sarr = np.array(grid, dtype=np.float64)
    idx = sband_vec(sarr)
    for x, i in zip(sarr, idx):
        if SLAB[int(i)] != sband_ref(float(x)):
            bad += 1
    print("    grid checked: %d PRB values, %d SNR values; mismatches: %d" % (305, len(grid), bad))
    if bad:
        fail("vectorised binning disagrees with the literal spec")


# ---------------------------------------------------------------------------------------------
# .npy header reader (keys + dtype + shape without decompressing the payload)
# ---------------------------------------------------------------------------------------------
def npy_header(f):
    ver = np.lib.format.read_magic(f)
    if ver == (1, 0):
        return np.lib.format.read_array_header_1_0(f)
    return np.lib.format.read_array_header_2_0(f)


def npz_headers(path):
    out = {}
    with zipfile.ZipFile(path) as zf:
        for nm in zf.namelist():
            if not nm.endswith(".npy"):
                continue
            with zf.open(nm) as f:
                shape, fortran, dtype = npy_header(f)
            out[nm[:-4]] = (dtype, tuple(shape))
    return out


def modpop_of(base):
    m = re.match(r"^" + re.escape(PREFIX) + r"_(QPSK|16QAM|64QAM|256QAM)_(self|harq)\.npz$", base)
    if not m:
        return None, None
    return m.group(1), m.group(2)


def mother_mod_pop(base):
    m = re.search(r"_(QPSK|16QAM|64QAM|256QAM)_(self|harq|harq_only)\.npz$", base)
    if not m:
        return None, None
    return m.group(1), ("harq" if m.group(2) in ("harq", "harq_only") else "self")


# ---------------------------------------------------------------------------------------------
def main():
    print("=" * 100)
    print("INDEPENDENT VERIFICATION OF %s_<MOD>_<pop>.npz" % PREFIX)
    print("=" * 100)
    selftest()

    # ---------------- 1. output files: counts, per-file bins, quota ----------------
    print("\n[1] output files (sample counts and bins re-derived from the arrays)")
    files = sorted(os.listdir(OUTDIR))
    out_files = [b for b in files if modpop_of(b)[0]]
    print("    %d npz in %s, %d of them match the expected name pattern" %
          (len([b for b in files if b.endswith(".npz")]), OUTDIR, len(out_files)))
    unexpected = [b for b in files if b.endswith(".npz") and not modpop_of(b)[0]]
    if unexpected:
        note("extra npz not matching the pattern: %s" % unexpected)

    draws = collections.Counter()          # (mod, w, s, pop) -> drawn
    per_file = {}                          # base -> dict
    rows = []
    for base in out_files:
        mod, pop = modpop_of(base)
        path = os.path.join(OUTDIR, base)
        z = np.load(path, allow_pickle=True)
        keys = list(z.files)
        dtypes, shapes = {}, {}
        qm = prb = snr = None
        for k in keys:
            a = z[k]
            dtypes[k] = str(a.dtype)
            shapes[k] = tuple(a.shape)
            if k == "qm":
                qm = np.array(a)
            elif k == "alloc_nof_prb":
                prb = np.array(a)
            elif k == "snr_nominal":
                snr = np.array(a)
            del a
        n = int(qm.shape[0])
        print("      loaded %-30s %10d samples, %d keys" % (base, n, len(keys)))
        # qm column must agree with the modulation in the file name
        qm_seen = {}
        for v in np.unique(qm):
            qm_seen[int(v)] = MOD_BY_QM.get(int(v), "UNMAPPED(%d)" % int(v))
        if set(qm_seen.values()) - {mod}:
            fail("%s: qm column maps to %s but the name says %s" % (base, qm_seen, mod))
        w = wband_vec(prb)
        s = sband_vec(snr)
        code = w.astype(np.int64) * 7 + s
        u, c = np.unique(code, return_counts=True)
        bins = {}
        for uu, cc in zip(u, c):
            wi, si = int(uu) // 7, int(uu) % 7
            bins[(WLAB[wi], SLAB[si])] = bins.get((WLAB[wi], SLAB[si]), 0) + int(cc)
            draws[(mod, WLAB[wi], SLAB[si], pop)] += int(cc)
        per_file[base] = {"mod": mod, "pop": pop, "n": n, "bins": bins, "keys": keys,
                          "dtypes": {k: str(z[k].dtype) for k in keys},
                          "shapes": {k: tuple(z[k].shape) for k in keys}, "qm_seen": qm_seen}
        mx = max(bins.values()) if bins else 0
        rows.append((base, n, mod, pop, len(bins), mx, QUOTA[pop]))
        if mx > QUOTA[pop]:
            fail("%s: a single bin holds %d samples, quota is %d" % (base, mx, QUOTA[pop]))
        z.close()

    print("    %-28s %10s %8s %10s %6s %6s %6s" %
          ("file", "samples", "mod", "pop", "bins", "maxbin", "quota"))
    for base, n, mod, pop, nb, mx, q in rows:
        print("    %-28s %10d %8s %10s %6d %6d %6d" % (base, n, mod, pop, nb, mx, q))

    total = sum(per_file[b]["n"] for b in per_file)
    print("\n    total drawn over all files: %d" % total)

    # per modulation / population
    by_mod = collections.Counter()
    by_pop = collections.Counter()
    by_modpop = collections.Counter()
    for b, d in per_file.items():
        by_mod[d["mod"]] += d["n"]
        by_pop[d["pop"]] += d["n"]
        by_modpop[(d["mod"], d["pop"])] += d["n"]
    for m in MODS:
        parts = ["%s=%d" % (p, by_modpop[(m, p)]) for p in ("self", "harq") if by_modpop[(m, p)]]
        print("    %-8s total %8d   (%s)" % (m, by_mod[m], ", ".join(parts) if parts else "none"))
    print("    self total %d, harq total %d" % (by_pop["self"], by_pop["harq"]))

    # ---------------- 2. per-bin quota check ----------------
    print("\n[2] per-(bin, population) quota check, re-derived from the output arrays")
    self_draw = [(v, k) for k, v in draws.items() if k[3] == "self"]
    harq_draw = [(v, k) for k, v in draws.items() if k[3] == "harq"]
    max_self = max([v for v, _ in self_draw], default=0)
    max_harq = max([v for v, _ in harq_draw], default=0)
    print("    max self drawn in one bin: %d (quota %d)" % (max_self, QUOTA["self"]))
    print("    max harq drawn in one bin: %d (quota %d)" % (max_harq, QUOTA["harq"]))
    if max_self > QUOTA["self"]:
        fail("a self bin exceeds 2500")
    if max_harq > QUOTA["harq"]:
        fail("a harq bin exceeds 250")
    nonzero_pairs = len(draws)
    print("    non-zero (bin, population) pairs: %d (expected <= 4*4*7*2 = 224, and <= 222 if one bin is empty)"
          % nonzero_pairs)
    nonzero_bins = len({(k[0], k[1], k[2]) for k in draws})
    print("    distinct bins with a non-zero draw: %d (of 112)" % nonzero_bins)
    if nonzero_pairs > 224:
        fail("more non-zero (bin,pop) pairs (%d) than bins exist" % nonzero_pairs)

    # ---------------- 3. mother-set recount (available) ----------------
    print("\n[3] mother-set recount (only qm / alloc_nof_prb / snr_nominal are read)")
    mother = [b for b in sorted(os.listdir(MOTHER)) if b.endswith(".npz")]
    avail = collections.Counter()
    mother_files = 0
    mother_rows = 0
    skipped = []
    unmapped_qm = 0
    for base in mother:
        mod_name, pop = mother_mod_pop(base)
        if mod_name is None or pop is None:
            skipped.append(base)
            continue
        z = np.load(os.path.join(MOTHER, base))
        try:
            qm = z["qm"]
            prb = z["alloc_nof_prb"]
            snr = z["snr_nominal"]
        finally:
            z.close()
        mother_files += 1
        n = int(qm.shape[0])
        mother_rows += n
        qmi = np.full(256, -1, dtype=np.int64)
        for q, nm in MOD_BY_QM.items():
            qmi[q] = MODS.index(nm)
        mi = qmi[qm.astype(np.int64)]
        unmapped_qm += int(np.count_nonzero(mi < 0))
        w = wband_vec(prb)
        s = sband_vec(snr)
        ok = mi >= 0
        code = mi[ok] * 28 + w[ok] * 7 + s[ok]
        u, c = np.unique(code, return_counts=True)
        for uu, cc in zip(u, c):
            mod = MODS[int(uu) // 28]
            wi = (int(uu) % 28) // 7
            si = int(uu) % 7
            avail[(mod, WLAB[wi], SLAB[si], pop)] += int(cc)
    print("    %d mother files, %d rows, %d name(s) skipped, %d row(s) with unmapped qm"
          % (mother_files, mother_rows, len(skipped), unmapped_qm))
    if skipped:
        note("skipped names: %s" % skipped)

    empty_bins = []
    for m in MODS:
        for w in WLAB:
            for s in SLAB:
                if avail[(m, w, s, "self")] + avail[(m, w, s, "harq")] == 0:
                    empty_bins.append((m, w, s))
    print("    bins with zero availability in the mother set: %d %s" % (len(empty_bins), empty_bins))
    avail_bins = len({(k[0], k[1], k[2]) for k, v in avail.items() if v})
    print("    bins with a non-zero availability: %d (of 112)" % avail_bins)

    short = []
    over = []
    for key, a in avail.items():
        d = draws.get(key, 0)
        want = min(a, QUOTA[key[3]])
        if d < want:
            short.append((key, a, d, want))
        if d > want:
            over.append((key, a, d, want))
    print("    bins drawing LESS than min(availability, quota): %d" % len(short))
    for key, a, d, want in sorted(short)[:50]:
        print("      %-40s available=%-8d drawn=%-8d expected>=%d" % (key, a, d, want))
    if short:
        fail("%d (bin,pop) short of min(availability, quota)" % len(short))
    if over:
        fail("%d (bin,pop) above min(availability, quota): %s" % (len(over), sorted(over)[:10]))

    below = [(k, a) for k, a in avail.items() if a < QUOTA[k[3]]]
    bad_eq = [(k, a, draws.get(k, 0)) for k, a in below if draws.get(k, 0) != a]
    print("    bins whose availability is BELOW quota: %d; of those, drawn != available: %d"
          % (len(below), len(bad_eq)))
    for k, a, d in sorted(bad_eq)[:20]:
        print("      %-40s available=%-8d drawn=%d" % (k, a, d))
    if bad_eq:
        fail("some bin with availability < quota did not draw everything")

    # bins available in the mother set but absent from the outputs entirely
    missing_bins = [k for k, a in avail.items() if a and k not in draws]
    if missing_bins:
        fail("%d available (bin,pop) pairs produced no draw: %s" % (len(missing_bins), sorted(missing_bins)[:10]))

    # ---------------- 4. manifest reconciliation ----------------
    print("\n[4] manifest reconciliation")
    mpath = os.path.join(OUTDIR, "manifest_%s.json" % PREFIX)
    if not os.path.exists(mpath):
        fail("manifest missing: %s" % mpath)
        man = None
    else:
        with open(mpath) as fh:
            man = json.load(fh)
        print("    manifest: %d bytes, keys=%s" % (os.path.getsize(mpath), sorted(man)))
        print("    dir=%s seed=%s quota_self=%s quota_harq=%s drawn_total=%s"
              % (man.get("dir"), man.get("seed"), man.get("quota_self"),
                 man.get("quota_harq"), man.get("drawn_total")))
        if man.get("quota_self") != QUOTA["self"] or man.get("quota_harq") != QUOTA["harq"]:
            fail("manifest quotas are not 2500/250")
        if man.get("drawn_total") != total:
            fail("manifest drawn_total=%s but the files hold %d samples" % (man.get("drawn_total"), total))
        wmap = {w.get("file"): w.get("samples") for w in man.get("written", [])}
        for base, d in per_file.items():
            if wmap.get(base) != d["n"]:
                fail("manifest written[] says %s -> %s, file holds %d"
                     % (base, wmap.get(base), d["n"]))
        man_bins = man.get("bins", [])
        print("    manifest records %d bins" % len(man_bins))
        disagree = []
        man_avail = {}
        for e in man_bins:
            key = (e["modulation"], e["width"], e["snr"], e["population"])
            man_avail[key] = e["available"]
            mine = draws.get(key, 0)
            if e["drawn"] != mine:
                disagree.append((key, e["drawn"], mine))
            if e["available"] != avail.get(key, 0):
                disagree.append((key, "available=%d" % e["available"], "recount=%d" % avail.get(key, 0)))
            if e["quota"] != QUOTA[key[3]]:
                disagree.append((key, "quota=%d" % e["quota"], "expected=%d" % QUOTA[key[3]]))
        print("    bins where the manifest disagrees with the recount: %d" % len(disagree))
        for k, a, b in disagree[:30]:
            print("      %-40s manifest=%-14s recount=%s" % (k, a, b))
        if disagree:
            fail("manifest disagrees with the recount in %d place(s)" % len(disagree))
        # every non-zero output bin must be in the manifest, and vice versa
        extra = [k for k in draws if k not in man_avail]
        if extra:
            fail("output has draws for bins the manifest does not list: %s" % sorted(extra)[:10])
        lost = [k for k, a in man_avail.items() if a > 0 and k not in avail]
        if lost:
            fail("manifest lists availability for bins my recount does not see: %s" % sorted(lost)[:10])

    # ---------------- 5. keys / dtypes ----------------
    print("\n[5] key and dtype preservation (input headers vs output arrays)")
    in_keys = collections.defaultdict(set)     # key -> set of dtype.str seen
    in_shape_tail = collections.defaultdict(set)
    in_total = 0
    for base in mother:
        try:
            h = npz_headers(os.path.join(MOTHER, base))
        except Exception as exc:      # noqa: BLE001
            fail("cannot read headers of %s: %r" % (base, exc))
            continue
        for k, (dt, shape) in h.items():
            in_keys[k].add(str(dt))
            in_shape_tail[k].add(tuple(shape[1:]))
        if "qm" in h:
            in_total += h["qm"][1][0]
    print("    union of input keys (%d): %s" % (len(in_keys), ", ".join(sorted(in_keys))))
    for k in sorted(in_keys):
        if len(in_keys[k]) > 1:
            note("input dtype for %s is not uniform: %s" % (k, sorted(in_keys[k])))
    out_keys = set()
    for base, d in per_file.items():
        out_keys |= set(d["keys"])
    print("    union of output keys (%d): %s" % (len(out_keys), ", ".join(sorted(out_keys))))
    missing = sorted(set(in_keys) - out_keys)
    print("    input keys MISSING from the outputs: %s" % (missing if missing else "none"))
    if missing:
        fail("keys dropped by the sampler: %s" % missing)
    extra_keys = sorted(out_keys - set(in_keys))
    if extra_keys:
        note("output-only keys: %s" % extra_keys)
    mism = []
    for base, d in per_file.items():
        for k, dt in d["dtypes"].items():
            if k in in_keys and dt not in in_keys[k]:
                mism.append((base, k, dt, sorted(in_keys[k])))
    print("    dtype mismatches: %d" % len(mism))
    for b, k, dt, exp in mism[:20]:
        print("      %-28s %-16s out=%s in=%s" % (b, k, dt, exp))
    if mism:
        fail("dtype mismatch in %d place(s)" % len(mism))
    # shape tails: bits/tb_bits are allowed to be zero-padded wider
    changed = []
    for base, d in per_file.items():
        for k, sh in d["shapes"].items():
            tail = tuple(sh[1:])
            if k in in_shape_tail and tail not in in_shape_tail[k]:
                changed.append((base, k, tail, sorted(in_shape_tail[k])[:4]))
    print("    output shapes whose trailing dims differ from the inputs: %d" % len(changed))
    for b, k, tail, exp in changed[:12]:
        print("      %-28s %-12s out=%s in=%s" % (b, k, tail, exp))
    bad_change = [c for c in changed if c[1] not in ("bits", "tb_bits")]
    if bad_change:
        fail("unexpected shape change on keys other than bits/tb_bits: %s" % bad_change[:5])
    pad = [c for c in changed if c[1] in ("bits", "tb_bits")]
    if pad:
        note("%d padding change(s) on bits/tb_bits (expected; valid length is nof_bits)" % len(pad))

    # ---------------- 6. disk ----------------
    print("\n[6] disk")
    sizes = {b: os.path.getsize(os.path.join(OUTDIR, b)) for b in files
             if os.path.isfile(os.path.join(OUTDIR, b))}
    du = sum(sizes.values())
    sv = os.statvfs(OUTDIR)
    free = sv.f_bavail * sv.f_frsize
    totalb = sv.f_blocks * sv.f_frsize
    print("    output dir total: %d bytes (%.2f GiB) in %d file(s)"
          % (du, du / 2 ** 30, len(sizes)))
    print("    filesystem: %.2f GiB total, %.2f GiB free (%.1f%% free)"
          % (totalb / 2 ** 30, free / 2 ** 30, 100.0 * sv.f_bavail / sv.f_blocks))
    for b in sorted(sizes):
        print("      %-32s %10d bytes" % (b, sizes[b]))

    # ---------------- summary ----------------
    print("\n" + "=" * 100)
    if FAILS:
        print("RESULT: FAIL (%d finding(s))" % len(FAILS))
        for f in FAILS:
            print("  - " + f)
    else:
        print("RESULT: PASS")
        print("  every (bin, population) is within quota, every bin drew min(availability, quota),")
        print("  the manifest agrees with the recount, and all input keys/dtypes are present.")
    print("=" * 100)
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
