"""Build the w2v3c training set: w2v1 whole, plus every 2-PRB high-SNR sample s2_amc holds.

WHY THIS SHAPE. w2v1 has only 556 samples at 2 PRB / 256QAM, which is why that band stayed pathological; the s2_amc
captures hold thousands. The supplement is 256QAM x 2 PRB x snr>=22 dB.

★ THE EVALUATION LEG MUST NOT BE IN THE MOTHER SET. `s2_amc_ping_1102a` is the leg the probe and the ruler are drawn
from; it lives in `datasets_eval_reserved_1102a/` precisely so a glob of `datasets_full_s2_amc/*.npz` cannot pick it up.
Building an earlier variant that included it produced a probe gain that was 62% memorisation (plan section 117).

Result is 183 624 samples / 19 .npy / 23 280 036 776 B; the fingerprint
(`md5sum *.npy | sort | md5sum`) must be f6c27b8bae52f7d57d19476a2a95606d or the data is not the one the recipes
in section 118 were measured on.

Usage:
    python build_w2v3c.py --w2v1 ~/ai_depth3/datasets_train_w2v1 \
        --mother ~/ai_depth3/datasets_full_s2_amc --out ~/ai_depth3/datasets_train_w2v3c
"""
import argparse
import glob
import os

import numpy as np

FIELDS = ("X", "H", "bits", "tb_bits", "qm", "r", "rv", "alloc_first_prb", "alloc_nof_prb", "snr_nominal",
          "sigma2_over_r2", "nof_bits", "nof_tb_bits", "n_id", "rnti", "dmrs_bitmap", "pilot_ls_ratio",
          "uci_ack_re", "uci_ack_rvd_re", "uci_csi1_re", "uci_nof_harq_ack", "source", "unit")
SNR_MIN, PRB = 22.0, 2
UNIT = "unit" in FIELDS


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--w2v1", required=True)
    ap.add_argument("--mother", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--snr-min", type=float, default=SNR_MIN)
    args = ap.parse_args()

    def get(path, name):
        return np.asarray(np.load(os.path.join(path, name + ".npy"), mmap_mode="r"))

    print("reading w2v1 ...")
    base = {f: get(args.w2v1, f) for f in FIELDS}
    n0 = len(base["qm"])
    known = {str(u).split("@")[0] for u in base["unit"]}
    print(f"  w2v1 {n0:,}")

    picks = []
    for f in sorted(glob.glob(os.path.join(args.mother, "*.npz"))):
        with np.load(f, allow_pickle=True) as z:
            qm = np.asarray(z["qm"])
            prb = np.asarray(z["alloc_nof_prb"])
            snr = np.asarray(z["snr_nominal"], dtype="f8")
            sel = np.where((qm == 8) & (prb == PRB) & (snr >= args.snr_min))[0]
            if not sel.size:
                continue
            units = [str(u).split("@")[0] for u in z["unit"]]
            keep = [int(i) for i in sel if units[int(i)] not in known]
            if keep:
                picks.append((f, keep))
    print(f"  supplement: {len(picks)} files, {sum(len(k) for _, k in picks):,} samples")

    out = {f: [] for f in FIELDS}
    for f, keep in picks:
        with np.load(f, allow_pickle=True) as z:
            for name in FIELDS:
                if name in z.files:
                    out[name].append(np.asarray(z[name])[keep])
    os.makedirs(args.out, exist_ok=True)
    total = n0
    for name in FIELDS:
        if name not in base:
            continue
        extra = np.concatenate(out[name]) if out[name] else np.zeros((0,), dtype=base[name].dtype)
        merged = np.concatenate([base[name], extra])
        total = len(merged)
        np.save(os.path.join(args.out, name + ".npy"), merged)
        print(f"  wrote {name}: {merged.shape}")
    print(f"done {args.out}: {total:,} samples")


if __name__ == "__main__":
    main()
