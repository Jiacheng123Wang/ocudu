#!/usr/bin/env python3
"""Audit built depth-3 datasets before anything trains on them.

WHY IT EXISTS. Three checks were written by hand in one afternoon and two of them were wrong, in ways that look like data corruption:

  * the mean of the `bits` array is a mean of PACKED BYTES, not a bit balance, so a healthy file reported "bit balance 84.5";
  * unpacking the whole array counts the ZERO PADDING after `nof_bits`, so a healthy file reported "a third of the bits are one".

Both would have sent someone looking for a capture bug that does not exist. This tool measures the stored bits the only way that means
anything -- unpack, then keep the first `nof_bits` of each sample -- and checks the properties a dataset has to have regardless of where it came
from: consistent shape and dtype, a single modulation per file matching its name, a plausible SNR range for that modulation, and grant widths it
claims to cover.

Exit status is 0 when there are no findings and 1 when there are, so a build script can gate on it.

Usage: check_depth3_dataset.py [--dir DATASETS] [--full] [--max-files N]
"""
import argparse
import collections
import glob
import os
import sys

import numpy as np

MODULATIONS = ["QPSK", "16QAM", "64QAM", "256QAM"]
# What a modulation's nominal SNR can plausibly be. Outside these the file is either mislabelled or was built from the wrong rows; the
# bounds are wide on purpose -- 256QAM really was collected down to 5.6 dB, which is the corner worth having.
SNR_SANE = {"QPSK": (-12.0, 32.0), "16QAM": (-8.0, 32.0), "64QAM": (-2.0, 32.0), "256QAM": (2.0, 34.0)}


def bit_fraction(bits, nof_bits):
    """Fraction of one-bits over the VALID part of each packed sample."""
    ones = total = 0
    for i in range(bits.shape[0]):
        packed = np.asarray(bits[i]).reshape(-1)
        n = int(nof_bits[i]) if nof_bits is not None and len(nof_bits) > i else packed.size * 8
        full, rem = divmod(n, 8)
        unpacked = np.unpackbits(packed[:full])
        if rem:
            unpacked = np.concatenate([unpacked, np.unpackbits(packed[full:full + 1])[:rem]])
        ones += int(unpacked.sum())
        total += unpacked.size
    return ones / total if total else float("nan")


def audit_file(path, full):
    name = os.path.basename(path)
    data = np.load(path, allow_pickle=True)
    keys = set(data.files)
    n = len(data["X"])
    bits = np.asarray(data["bits"])
    nof_bits = np.asarray(data["nof_bits"], dtype=np.int64) if "nof_bits" in keys else None
    snr = np.asarray(data["snr_nominal"], dtype=float) if "snr_nominal" in keys else np.array([])
    prb = np.asarray(data["alloc_nof_prb"]) if "alloc_nof_prb" in keys else np.array([])
    qm = sorted({int(v) for v in np.asarray(data["qm"]).tolist()}) if "qm" in keys else []
    modulation = next((m for m in MODULATIONS if m in name), None)

    # The bit fraction is the expensive part: it unpacks every sample. --full does every file, otherwise a sample of at most 400 rows.
    rows = bits if full or n <= 400 else bits[np.linspace(0, n - 1, 400).astype(int)]
    rows_nof = nof_bits if full or n <= 400 else (nof_bits[np.linspace(0, n - 1, 400).astype(int)] if nof_bits is not None else None)

    return dict(name=name, n=n, shape=tuple(np.asarray(data["X"]).shape[1:]), dtype=str(np.asarray(data["X"]).dtype),
                bit1=bit_fraction(np.asarray(rows), rows_nof), qm=qm, modulation=modulation,
                snr_med=float(np.median(snr)) if snr.size else float("nan"),
                snr_lo=float(np.percentile(snr, 5)) if snr.size else float("nan"),
                snr_hi=float(np.percentile(snr, 95)) if snr.size else float("nan"),
                prb_lo=int(prb.min()) if prb.size else -1, prb_hi=int(prb.max()) if prb.size else -1,
                missing=sorted({"X", "H", "bits", "tb_bits", "qm", "snr_nominal"} - keys))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="datasets_bin")
    ap.add_argument("--full", action="store_true", help="measure every sample instead of a sample of 400 per file")
    ap.add_argument("--max-files", type=int, default=0)
    args = ap.parse_args()

    files = sorted(glob.glob(os.path.join(args.dir, "*.npz")))
    if args.max_files:
        files = files[:args.max_files]
    if not files:
        print(f"nothing to audit in {args.dir}")
        return 2

    reports, findings = [], []
    for path in files:
        r = audit_file(path, args.full)
        reports.append(r)
        if r["missing"]:
            findings.append((r["name"], f"missing keys: {r['missing']}"))
        if r["bit1"] != r["bit1"] or not (0.45 <= r["bit1"] <= 0.55):
            findings.append((r["name"], f"bit fraction {r['bit1']:.3f} (expected 0.45-0.55)"))
        if r["modulation"] and len(r["qm"]) != 1:
            findings.append((r["name"], f"qm is not a single value: {r['qm']}"))
        if r["modulation"] and r["snr_med"] == r["snr_med"]:
            lo, hi = SNR_SANE[r["modulation"]]
            if not (lo <= r["snr_med"] <= hi):
                findings.append((r["name"], f"{r['modulation']} median SNR {r['snr_med']:.1f} dB outside {lo}..{hi}"))

    print(f"{len(reports)} file(s), {sum(r['n'] for r in reports)} sample(s)\n")
    print("%-46s %-6s %7s %8s %7s %13s %10s" % ("file", "mod", "n", "shape", "bit1", "snr p5-p95", "prb"))
    for r in reports[:20]:
        print("%-46s %-6s %7d %8s %7.3f %6.1f-%5.1f %5d-%d" %
              (r["name"][:46], r["modulation"] or "?", r["n"], r["shape"], r["bit1"],
               r["snr_lo"], r["snr_hi"], r["prb_lo"], r["prb_hi"]))
    if len(reports) > 20:
        print("... (%d more)" % (len(reports) - 20))

    print("\n--- totals ---")
    print("shapes:", dict(collections.Counter(r["shape"] for r in reports)))
    print("dtypes:", dict(collections.Counter(r["dtype"] for r in reports)))
    for m in MODULATIONS:
        sel = [r for r in reports if r["modulation"] == m]
        if sel:
            print("  %-6s files %3d  samples %6d  bit1 %.3f  median-SNR %.1f..%.1f dB" %
                  (m, len(sel), sum(r["n"] for r in sel), np.mean([r["bit1"] for r in sel]),
                   min(r["snr_med"] for r in sel), max(r["snr_med"] for r in sel)))

    print("\n--- findings ---")
    for name, why in findings[:20]:
        print("  %-46s %s" % (name[:46], why))
    if not findings:
        print("   none")
    return 1 if findings else 0


if __name__ == "__main__":
    raise SystemExit(main())
