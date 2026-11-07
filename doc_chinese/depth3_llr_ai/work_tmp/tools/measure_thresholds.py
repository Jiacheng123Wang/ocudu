#!/usr/bin/env python3
"""Measure the per-modulation LDPC thresholds on the NOMINAL signal-to-noise ratio, from the corpus.

WHY THIS EXISTS. The training-side weighting (`--snr-weight` in `train_depth3.py`) needs to know, for each modulation, where
the decoder's block-error rate crosses 50% -- because that is where a reception is decided and therefore where the delivered
bit count is won or lost. Textbook numbers do not apply: the threshold depends on our code rate, our allocation sizes and our
demodulator, not on the modulation alone. So it is measured, and re-measured whenever the corpus grows.

TWO SCALES, AND ONLY ONE OF THEM IS RIGHT. The dump index carries the estimator's MEASURED ratio; the datasets carry the
NOMINAL one. They differ by up to nine decibels on QPSK, and the training loop only ever sees the nominal one, so a threshold
taken from the measured scale would misplace the weighting window for every modulation.

THE JOIN. The mother sets carry the capture id, the modulation and the nominal ratio per sample; the dump index carries the
classical chain's CRC verdict per capture. Joining them by id, within a leg, gives one verdict per reception with both ratios.
No raw captures are needed -- only the mother `.npz` files and the `dump_s2/*/index.csv` files.

LEGS ARE NOT INTERCHANGEABLE. A per-leg estimate of the 256QAM crossing spans 17.8 to 23.5 dB over thirty-two legs, so a single
leg is off by more than a decibel; the pooled estimate is the one to use. The corpus has two families: `s2_amc_*` (the scheduler
picks the MCS, all four modulations) and `s2_mcs*` (fixed MCS, QPSK only). They are reported apart because they are different
experiments, and because the per-modulation statistic cannot be disturbed by legs of another modulation.

MEASURED 2026-11-07 (this script): QPSK -3.16, 16QAM 6.61, 64QAM 13.38, 256QAM 20.91 dB on 1,890,606 receptions.
"""
import argparse
import collections
import csv
import glob
import os

import numpy as np

QM_NAME = {2: "QPSK", 4: "16QAM", 6: "64QAM", 8: "256QAM"}


def legs_of(mother, pattern):
    names = {os.path.basename(p).split("_QPSK")[0].split("_16QAM")[0].split("_64QAM")[0].split("_256QAM")[0]
             for p in glob.glob(os.path.join(mother, pattern + "*.npz"))}
    return sorted(names)


def collect(mother, dumps, legs):
    """(leg, modulation, snr_nominal, crc_ok) for every reception whose verdict is known, deduplicated by capture."""
    rows, seen, skipped = [], set(), []
    for leg in legs:
        index = os.path.join(dumps, "tb_" + leg, "index.csv")
        if not os.path.exists(index):
            skipped.append(leg)
            continue
        verdict = {r["unit"]: r["crc"] == "OK" for r in csv.DictReader(open(index))}
        for f in sorted(glob.glob(os.path.join(mother, leg + "_*.npz"))):
            z = np.load(f, allow_pickle=True)
            units, qm, snr = z["unit"], z["qm"], z["snr_nominal"]
            for i in range(len(units)):
                u = str(units[i]).split("@")[0]
                if u in seen or u not in verdict:
                    continue
                seen.add(u)
                rows.append((leg, QM_NAME[int(qm[i])], float(snr[i]), verdict[u]))
    if skipped:
        # COUNTED, NOT FORGOTTEN: a leg skipped by a path mistake is indistinguishable from a leg with no data unless the
        # number is printed, which is how twenty-five legs went missing on the first run of this analysis.
        print(f"skipped {len(skipped)} leg(s) with no dump index: {', '.join(skipped[:4])}"
              f"{' ...' if len(skipped) > 4 else ''}")
    return rows


def curve(rows, modulation, step=0.5, min_bin=30):
    """Block-error rate against the nominal ratio, with a monotone envelope.

    The envelope is not cosmetic: sparse bins at the edges of the captured range produce non-monotone points, and a crossing
    found through one of them lands anywhere. Requiring the success rate never to fall with the ratio removes them.
    """
    bins = collections.defaultdict(lambda: [0, 0])
    for _, m, snr, ok in rows:
        if m != modulation:
            continue
        k = round(snr / step) * step
        bins[k][0] += 1
        bins[k][1] += 1 if ok else 0
    out, best = [], 0.0
    for k, (n, ok) in sorted(bins.items()):
        if n < min_bin:
            continue
        best = max(best, ok / n)
        out.append((k, best, n))
    return out


def crossing(points, target):
    for (b0, r0, _), (b1, r1, _) in zip(points, points[1:]):
        if (r0 - target) * (r1 - target) <= 0 and r1 != r0:
            return b0 + (target - r0) / (r1 - r0) * (b1 - b0)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mother", default=os.path.expanduser("~/ai_depth3/datasets_full_s2"))
    ap.add_argument("--dumps", default=os.path.expanduser("~/ai_depth3/dump_s2"))
    ap.add_argument("--step", type=float, default=0.5)
    ap.add_argument("--min-bin", type=int, default=30)
    args = ap.parse_args()

    amc = collect(args.mother, args.dumps, legs_of(args.mother, "s2_amc_"))
    mcs = collect(args.mother, args.dumps, legs_of(args.mother, "s2_mcs"))
    print(f"AMC legs: {len(amc):,} receptions   MCS legs: {len(mcs):,} receptions")
    mods_mcs = sorted({m for _, m, _, _ in mcs})
    print(f"modulations in the MCS family: {mods_mcs or 'none'}")

    print(f"\n{'modulation':>10} {'BLER90%':>9} {'BLER50%':>9} {'AMC only':>9} {'samples':>10}")
    for mod in ("QPSK", "16QAM", "64QAM", "256QAM"):
        p_all = curve(amc + mcs, mod, args.step, args.min_bin)
        p_amc = curve(amc, mod, args.step, args.min_bin)
        n = sum(x[2] for x in p_all)
        f = lambda x: f"{x:>9.2f}" if x is not None else f"{'n/a':>9}"
        print(f"{mod:>10} {f(crossing(p_all, 0.9))} {f(crossing(p_all, 0.5))} "
              f"{f(crossing(p_amc, 0.5))} {n:>10,}")

    print("\nper-leg 256QAM crossing (a single leg is not enough):")
    per = collections.defaultdict(list)
    for leg, m, snr, ok in amc + mcs:
        if m == "256QAM":
            per[leg].append((snr, ok))
    vals = []
    for leg in sorted(per):
        if len(per[leg]) < 500:
            continue
        t = crossing(curve([(leg, "256QAM", s, ok) for s, ok in per[leg]], "256QAM", args.step, 20), 0.5)
        if t is not None:
            vals.append(t)
    if vals:
        print(f"  {len(vals)} legs: {min(vals):.2f} .. {max(vals):.2f} dB, "
              f"mean {sum(vals) / len(vals):.2f}, sd {(sum((v - sum(vals) / len(vals)) ** 2 for v in vals) / len(vals)) ** 0.5:.2f}")


if __name__ == "__main__":
    main()
