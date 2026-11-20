"""Build a held-out monitor set from the 1102a evaluation corpus, excluding the probe and ruler units.

WHY. `datasets_monitor_w2` is 37% overlapping with the training set (270 of its 730 samples appear in w2v3c), so the
val number it produces is partly memorisation and cannot select anything -- which is consistent with the ten recorded
inversions between val and delivered bits. The 1102a corpus is the evaluation-reserved one: it is NOT in any training
set, and the probe/ruler draws come from it. Excluding those two draws keeps the monitor independent of BOTH the
training data and the measurement sets, so a periodic BLER check on it says something about generalisation.

STRATIFICATION. Uniform sampling would be dominated by the 256QAM mass. The set is sampled per (modulation, PRB class,
SNR band) so the monitor has at least a few narrow-band low-SNR units, which is where the model actually fails.

Usage:
    python build_monitor_1102a.py --out ~/ai_depth3/datasets_monitor_1102a [--per-stratum 4]
"""
import argparse
import glob
import os

import numpy as np

# THE BIT FIELDS ARE PADDED TO THE WIDTHS THE TRAINING SET USES. The reserved npz store `bits` at each modulation's
# own length (1836/3456/4320/5508) and `tb_bits` at each transport block's, while the training arrays are fixed width
# (7344 and 6787) with the tail zero. `nof_bits` says how much of the row is real, so zero padding is exact.
PAD = {"bits": 7344, "tb_bits": 6787}
FIELDS = ("X", "H", "bits", "tb_bits", "qm", "rv", "alloc_first_prb", "alloc_nof_prb", "snr_nominal",
          "sigma2_over_r2", "nof_bits", "nof_tb_bits", "n_id", "rnti", "dmrs_bitmap",
          "uci_ack_re", "uci_ack_rvd_re", "uci_csi1_re", "uci_nof_harq_ack")
HOME = os.path.expanduser("~")
RESERVED = os.path.join(HOME, "ai_depth3/datasets_eval_reserved_1102a")
PROBE = os.path.join(HOME, "ai_depth3/exp_bits_1102/units_probe_500.txt")
RULER = os.path.join(HOME, "dev/ocudu/lib/phy/upper/ai_signal_processors/test/ruler_s2_amc_5000_units.txt")


def snr_band(db):
    for hi in (10.0, 18.0, 24.0, 30.0):
        if db < hi:
            return f"<{hi:g}"
    return ">=30"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--per-stratum", type=int, default=4)
    ap.add_argument("--seed", type=int, default=20261120)
    args = ap.parse_args()

    probe = {l.strip() for l in open(PROBE) if l.strip()}
    ruler = {l.strip() for l in open(RULER) if l.strip()}
    excluded = probe | ruler

    # collect every reserved sample, tagged by its stratum
    rows = []
    for path in sorted(glob.glob(os.path.join(RESERVED, "*.npz"))):
        with np.load(path, allow_pickle=True) as z:
            units = [str(x).split("@")[0] for x in z["unit"]]
            for i, unit in enumerate(units):
                if unit in excluded:
                    continue
                qm = int(z["qm"][i])
                prb = int(z["alloc_nof_prb"][i])
                rows.append((snr_band(float(z["snr_nominal"][i])), qm, "narrow" if prb <= 3 else "wide", path, i, unit))
    print(f"candidates after excluding probe+ruler: {len(rows)}")

    rng = np.random.default_rng(args.seed)
    strata = {}
    for r in rows:
        strata.setdefault(r[:3], []).append(r)
    picked = []
    for key in sorted(strata):
        bucket = strata[key]
        idx = rng.permutation(len(bucket))[: args.per_stratum]
        picked += [bucket[i] for i in idx]
    print(f"strata: {len(strata)}, picked: {len(picked)}")

    os.makedirs(args.out, exist_ok=True)
    arrays = {name: [] for name in FIELDS}
    for snr, qm, width, path, i, unit in picked:
        with np.load(path, allow_pickle=True) as z:
            for name in FIELDS:
                if name not in z:
                    raise SystemExit(f"{os.path.basename(path)} lacks field {name}")
                value = np.asarray(z[name][i])
                if name in PAD and value.shape[0] < PAD[name]:
                    padded = np.zeros((PAD[name],) + value.shape[1:], dtype=value.dtype)
                    padded[: value.shape[0]] = value
                    value = padded
                arrays[name].append(value)
    for name in FIELDS:
        np.save(os.path.join(args.out, name + ".npy"), np.stack(arrays[name]))
    print(f"wrote {len(picked)} samples x {len(FIELDS)} fields to {args.out}")
    print("total bytes:", sum(os.path.getsize(os.path.join(args.out, f + ".npy")) for f in FIELDS))


if __name__ == "__main__":
    main()
