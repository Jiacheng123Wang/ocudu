#!/usr/bin/env python3
"""Census of the training set by DEPLOYMENT BUCKET, to decide how many macro-experts the width axis can support.

WHY THIS EXISTS. The runtime has seven static input widths (`base_w108..w540`, plus the 612 full band) because the ANE
needs a fixed shape; they all share ONE weight set today. Turning those seven SHAPES into seven EXPERTS would multiply
the weight sets by seven, and whether that is affordable is a data question, not a shape question: an expert trained on
1 500 samples cannot be judged on the same footing as one trained on 86 000. This script prints the sample/bit/256QAM
census per bucket and the recommended narrow/wide split, from the same columns the trainer reads.

Usage:
  expert_census.py <dataset_dir> [--json out.json]
"""
import argparse
import json
import os

import numpy as np

# The bucket edges the runtime uses, inverted: needed_nsc = 12*(prb+8) is fitted to the smallest of these widths, so a
# reception lands in bucket w when 12*(prb+8) <= w. Edge clipping at the band ends can make a reception land one bucket
# narrower, which is why the bucket index is NOT used as the expert label here; the PRB count is.
BUCKETS = ((108, 1, 1), (180, 2, 7), (252, 8, 13), (324, 14, 19),
           (396, 20, 25), (468, 26, 31), (540, 32, 37), (612, 38, 51))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dataset")
    ap.add_argument("--json", default="")
    args = ap.parse_args()

    def load(name):
        return np.load(os.path.join(args.dataset, name + ".npy"))

    prb = load("alloc_nof_prb")
    qm = load("qm")
    bits = load("nof_tb_bits").astype(np.int64)
    snr = load("snr_nominal")
    total_bits = int(bits.sum())
    rows = []
    for width, lo, hi in BUCKETS:
        mask = (prb >= lo) & (prb <= hi)
        rows.append({"bucket": f"w{width}", "prb_lo": lo, "prb_hi": hi,
                     "samples": int(mask.sum()), "sample_share": float(mask.mean()),
                     "bits": int(bits[mask].sum()), "bit_share": float(bits[mask].sum() / total_bits),
                     "qam256": int((qm[mask] == 8).sum()),
                     "snr_median": float(np.median(snr[mask])) if mask.any() else None})
    print(f"{'bucket':>7} {'PRB':>7} {'samples':>8} {'sample%':>8} {'bits%':>7} {'256QAM':>7} {'snr med':>7}")
    for row in rows:
        print(f"{row['bucket']:>7} {row['prb_lo']:>3}-{row['prb_hi']:<3} {row['samples']:>8} "
              f"{100 * row['sample_share']:>7.1f}% {100 * row['bit_share']:>6.1f}% {row['qam256']:>7} "
              f"{row['snr_median']:>7.1f}")
    print("\n2-expert split (the architecture's Layer 1):")
    splits = []
    for name, mask in (("narrow <= 8 PRB", prb <= 8), ("wide > 8 PRB", prb > 8)):
        splits.append({"name": name, "samples": int(mask.sum()),
                       "sample_share": float(mask.mean()), "bit_share": float(bits[mask].sum() / total_bits),
                       "qam256": int((qm[mask] == 8).sum())})
        print(f"  {name:18s} {int(mask.sum()):>8} ({100 * mask.mean():5.1f}%)  "
              f"bits {100 * bits[mask].sum() / total_bits:5.1f}%  256QAM {int((qm[mask] == 8).sum())}")
    if args.json:
        with open(args.json, "w") as handle:
            json.dump({"buckets": rows, "two_expert": splits}, handle, indent=2)
        print(f"wrote {args.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
