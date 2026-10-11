#!/usr/bin/env python3
"""Measure how much a PERFECT channel estimate would be worth, on a synthetic grid.

Why this number decides the experiment
--------------------------------------
If a network is trained to refine the chain's channel estimate, its value is bounded by what a
perfect estimate would buy. That bound is not a modelling choice - it can be measured - and it
is the number that says whether the whole exercise is worth doing.

The measurement is a paired comparison on ONE grid, so nothing else varies:

    grid        = H_true * X + noise, noise variance known by construction
    variant A   = equalise with H_true                      (the oracle)
    variant B   = equalise with the chain's own estimate    (the baseline)

Both use the same MMSE form and the same noise variance, so the difference is the channel
estimate alone. The comparison is restricted to the interior of the allocation: the allocation
here sits flush against the upper edge of the BWP, and the chain's estimate degrades sharply
over the last few percent of subcarriers (measured in the design record, section 36.4). Leaving
them in inflates the oracle's apparent value from ~1.2 dB to ~5.2 dB, so the trim is reported
explicitly rather than chosen silently.

A caveat that matters for interpretation: the "true" channel here is the one recorded next to a
real capture, which is itself the chain's estimate from that capture. It is a genuine, smooth,
physically plausible channel, but it is not ground truth in the absolute sense. What the tool
measures is therefore "what is left on the table for a better estimator on THIS channel", which
is the right question for the experiment even though the channel carries a little of the
chain's own error.

Usage
-----
    measure_oracle_gain.py --unit C_1777_17921 --corpus <dir> --tb <dir> [--snr 20]
"""

import argparse
import glob
import os
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)


def _find_repo(start):
    """Walk up to the directory holding `build/`: counting ".." levels breaks when this file is
    imported from a copy elsewhere, and that failure is silent (it runs a nonexistent binary)."""
    path = start
    for _ in range(12):
        if os.path.isdir(os.path.join(path, "build")) and os.path.isdir(os.path.join(path, "lib")):
            return path
        parent = os.path.dirname(path)
        if parent == path:
            break
        path = parent
    raise SystemExit(f"cannot locate the repository root from {start}")


REPO = _find_repo(HERE)
REPLAY = os.path.join(REPO, "build", "lib", "phy", "upper", "channel_processors",
                      "metal", "ul_chain_replay")

import synth as SN       # noqa: E402
import dmrs as DV        # noqa: E402


def chain_estimate(cfg, grid, staged_prefix, source_prefix):
    """Run the real receive path over `grid` and return the channel estimate it produces."""
    os.makedirs(os.path.dirname(staged_prefix), exist_ok=True)
    grid.astype(np.complex64).tofile(staged_prefix + ".bin")
    cfg_channel = np.zeros_like(grid)
    cfg_channel.astype(np.complex64).tofile(staged_prefix + "_h.bin")
    with open(source_prefix + ".txt") as src, open(staged_prefix + ".txt", "w") as dst:
        dst.write(src.read())
    subprocess.run([REPLAY, staged_prefix, "--metal", "--out", staged_prefix + "_out"],
                   check=False, capture_output=True)
    found = glob.glob(staged_prefix + "_out*_h.bin")
    if not found:
        return None
    return np.fromfile(found[0], dtype=np.complex64)


def equalised_symbol_error(channel, grid, symbols, positions, subcarrier_offset, noise_variance,
                           lo, hi):
    """Mean squared error of the MMSE-equalised data symbols, over positions in [lo, hi).

    MMSE form: x_hat = conj(h) * y / (|h|^2 + sigma^2). Using the same expression for both
    variants is what makes the comparison about the channel alone.
    """
    errors = []
    for index, (symbol, subcarrier) in enumerate(positions):
        if not (lo <= subcarrier < hi):
            continue
        absolute = subcarrier + subcarrier_offset
        estimate = channel[symbol, absolute]
        x_hat = (np.conj(estimate) * grid[symbol, absolute]
                 / (np.abs(estimate) ** 2 + noise_variance))
        errors.append(abs(x_hat - symbols[index]) ** 2)
    return float(np.mean(errors)), len(errors)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--unit", required=True)
    parser.add_argument("--corpus", required=True)
    parser.add_argument("--tb", required=True)
    parser.add_argument("--snr", type=float, default=20.0)
    parser.add_argument("--seed", type=int, default=20261010)
    parser.add_argument("--staged", default="/tmp/oracle_gain")
    args = parser.parse_args()

    prefix = os.path.join(args.corpus, args.unit)
    cfg = SN.read_config(prefix)
    bwp = int(cfg["bwp_size_rb"])
    alloc = [int(x) for x in cfg["alloc_prb"].split(",")]
    dmrs_syms = [int(x) for x in cfg["dmrs_symbols"].split(",")]
    dmrs_mask = sum(1 << s for s in dmrs_syms)
    mod = {"QPSK": 2, "16QAM": 4, "64QAM": 6, "256QAM": 8}[cfg["modulation"]]

    H = np.fromfile(prefix + "_h.bin", dtype=np.complex64).reshape(SN.NOF_SYMBOLS, bwp * 12)
    tb_bits = np.unpackbits(np.fromfile(os.path.join(args.tb, args.unit + ".bits"),
                                        dtype=np.uint8))
    symbols, positions = SN.encode_symbols(tb_bits, len(alloc), SN.NOF_SYMBOLS, mod, dmrs_mask,
                                          int(cfg["n_id"]), int(cfg["rnti"]), int(cfg["rv"]))

    clean = SN.build_clean(cfg, H, symbols, positions, alloc, dmrs_syms)
    grid, noise_variance = SN.add_noise(clean, args.snr, args.seed)

    print(f"unit {args.unit}: {len(alloc)} PRB, {cfg['modulation']}, slot {cfg['slot']}")
    print(f"target SNR {args.snr} dB -> noise variance {noise_variance:.6g}")

    estimate = chain_estimate(cfg, grid, os.path.join(args.staged, args.unit), prefix)
    if estimate is None:
        raise SystemExit("the replay produced no channel estimate")
    estimate = estimate.reshape(SN.NOF_SYMBOLS, bwp * 12)

    # The chain applies a scale convention of its own, so its estimate is compared after the
    # single complex gain that best matches the two. A constant scale is irrelevant to the
    # demapper, which estimates a gain anyway.
    subcarriers = np.concatenate([np.arange(r * 12, (r + 1) * 12) for r in alloc])
    reference = H[:, subcarriers].ravel()
    measured = estimate[:, subcarriers].ravel()
    gain = np.vdot(reference, measured) / np.vdot(reference, reference)
    print(f"chain scale relative to the true channel: 1/{1 / abs(gain):.4f} "
          f"(a convention, removed before comparing)")

    offset = min(alloc) * 12
    width = len(alloc) * 12
    print()
    print("  trim   subcarriers   true channel   chain estimate    oracle gain")
    baseline = None
    for fraction in (0.0, 0.02, 0.05, 0.10, 0.20):
        lo, hi = int(width * fraction), width - int(width * fraction)
        oracle, count = equalised_symbol_error(H, grid, symbols, positions, offset,
                                               noise_variance, lo, hi)
        actual, _ = equalised_symbol_error(estimate / gain, grid, symbols, positions, offset,
                                           noise_variance, lo, hi)
        print(f"  {fraction * 100:4.0f}%   {count:11d}   {oracle:.6f}      {actual:.6f}"
              f"     {10 * np.log10(actual / oracle):+6.2f} dB")
        if abs(fraction - 0.05) < 1e-9:
            baseline = 10 * np.log10(actual / oracle)

    print()
    print(f"=> oracle gain at a 5% trim: {baseline:+.2f} dB")
    print("   This is the ceiling for any refinement network on this channel: it is what a")
    print("   PERFECT estimate buys. The edge figure is much larger and is an artefact of the")
    print("   allocation sitting against the BWP edge.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
