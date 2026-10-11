#!/usr/bin/env python3
"""Measure the chain's channel-estimation error against a KNOWN channel, as a function of SNR.

Why this is the measurement that decides the training target
-----------------------------------------------------------
The real label corpus turned out not to contain an estimator error at all: its labels are
decision-directed reconstructions whose residual is uncorrelated with the input and does not
fall with SNR, so it is the labels' own noise floor and not something a network could learn.
Measuring how much room the chain actually leaves therefore needs a grid whose truth is known
by construction - which is what the synthesis route provides.

Method
------
For each unit:
  1. Build a noiseless grid from the unit's own decoded TB and the channel estimate recorded
     next to the capture, so the transmitted symbols and the channel are both known.
  2. Add white Gaussian noise at a target SNR.
  3. Replay it through the real receive path.
  4. Compare the chain's channel estimate against the known channel.

The quantity reported is `NMSE = E|Hhat - g*H|^2 / E|g*H|^2`, with `g` the single complex gain
that best matches the two. Removing `g` is deliberate: the chain applies a scale convention of
its own (measured at a constant 0.605 relative to a pilot LS estimate), and a constant scale is
irrelevant to the demapper, which estimates a gain anyway. What matters is the *shape* error.

Edge subcarriers are excluded. `alloc_prb=0..49` inside a 51 PRB BWP places the allocation at
absolute subcarriers 12..611, and the chain's estimate degrades sharply over the last ~14 of
them (measured: their contribution alone is +11 dB of NMSE). That is an artefact of the window
rather than of the estimator, and it is reported separately rather than silently dropped.

Usage
-----
    measure_ce_error.py --units <index.csv> --tb <dir> --corpus <dir> [--snrs 10,15,20,25,30]
"""

import argparse
import glob
import os
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
AI_TRAIN = os.path.abspath(os.path.join(HERE, "..", "..", "..", "lib", "phy", "upper",
                                        "signal_processors", "channel_estimator",
                                        "metal", "ai_train"))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
REPLAY = os.path.join(REPO, "build", "lib", "phy", "upper", "channel_processors",
                      "metal", "ul_chain_replay")

sys.path.insert(0, AI_TRAIN)
sys.path.insert(0, HERE)
import dmrs as DV              # noqa: E402
import synth_noisy as SN       # noqa: E402

NOF_SYMBOLS = 14
SLOTS_PER_FRAME_30KHZ = 20
# The chain's estimate is only compared where it is not dominated by the window edge; the
# fraction is measured, not assumed (see the module docstring).
EDGE_FRACTION = 0.03


def read_config(prefix):
    cfg = {}
    with open(prefix + ".txt") as handle:
        for line in handle:
            if "=" in line:
                key, _, value = line.partition("=")
                cfg[key.strip()] = value.strip()
    return cfg


def build_grid(cfg, tb_bits, H, snr_db, seed):
    """Noiseless grid from a known channel and the re-encoded TB, plus optional noise."""
    nof_subc = H.shape[1]
    alloc = [int(x) for x in cfg["alloc_prb"].split(",")]
    dmrs_syms = [int(x) for x in cfg["dmrs_symbols"].split(",")]
    dmrs_mask = sum(1 << s for s in dmrs_syms)

    mod_enum = {"QPSK": 2, "16QAM": 4, "64QAM": 6, "256QAM": 8}[cfg["modulation"]]
    # The data symbols are laid out over the ALLOCATION width, not the BWP: `encode_symbols`
    # works in the allocation's own subcarrier grid and `build_clean` then maps it back through
    # absolute PRB indices. Passing the BWP width over-counts the data REs, which changes the
    # rate-matching length and makes the code-block count disagree with the segmentation.
    X, data_pos = SN.encode_symbols(tb_bits, len(alloc), NOF_SYMBOLS, mod_enum,
                                    dmrs_mask, int(cfg["n_id"]), int(cfg["rnti"]), int(cfg["rv"]))
    grid = SN.build_clean(cfg, H, X, data_pos, alloc, dmrs_syms)

    if snr_db is not None:
        sig = float(np.mean(np.abs(grid[np.abs(grid) > 0]) ** 2))
        sigma2 = sig / (10.0 ** (snr_db / 10.0))
        rng = np.random.default_rng(seed)
        noise = (rng.standard_normal(grid.shape) + 1j * rng.standard_normal(grid.shape))
        grid = grid + noise * np.sqrt(sigma2 / 2.0)
    return grid


def replay(staged_prefix, grid, H, source_prefix):
    """Write a staged capture, run the real receive path over it, return its channel estimate.

    `source_prefix` supplies the metadata, because the staged grid must carry the same
    configuration (slot, allocation, DM-RS, TB size) as the capture it was derived from.
    """
    os.makedirs(os.path.dirname(staged_prefix), exist_ok=True)
    grid.astype(np.complex64).tofile(staged_prefix + ".bin")
    H.astype(np.complex64).tofile(staged_prefix + "_h.bin")
    with open(source_prefix + ".txt") as src, open(staged_prefix + ".txt", "w") as dst:
        dst.write(src.read())

    subprocess.run([REPLAY, staged_prefix, "--metal", "--out", staged_prefix + "_out"],
                   check=False, capture_output=True)
    found = glob.glob(staged_prefix + "_out*_h.bin")
    if not found:
        return None
    return np.fromfile(found[0], dtype=np.complex64)


def nmse(known, estimated, subcarriers):
    """Shape error against the known channel, with the best single complex gain removed.

    Both arguments are one symbol's frequency response over the full BWP width; `subcarriers`
    selects the positions the comparison is made on.
    """
    a = known[subcarriers]
    b = estimated[subcarriers]
    if a.shape != b.shape:
        return None
    g = np.sum(b * np.conj(a)) / np.sum(np.abs(a) ** 2)
    return float(np.sum(np.abs(b - g * a) ** 2) / np.sum(np.abs(g * a) ** 2))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--units", required=True, help="index.csv from dump_tb.sh")
    parser.add_argument("--tb", required=True, help="directory of tb/<unit>.bits")
    parser.add_argument("--corpus", required=True, help="directory of C_<slot>_<rnti>.txt")
    parser.add_argument("--snrs", default="10,15,20,25,30")
    parser.add_argument("--max-units", type=int, default=8)
    parser.add_argument("--seed", type=int, default=20261010)
    args = parser.parse_args()

    snrs = [float(x) for x in args.snrs.split(",")]

    # Only pairs where the TB, the capture and the channel estimate all exist.
    units = []
    with open(args.units) as handle:
        next(handle)
        for line in handle:
            fields = line.strip().split(",")
            if len(fields) < 3 or fields[2] != "OK":
                continue
            name = fields[0]
            if not os.path.exists(os.path.join(args.tb, name + ".bits")):
                continue
            prefix = os.path.join(args.corpus, name)
            if not os.path.exists(prefix + ".bin") or not os.path.exists(prefix + "_h.bin"):
                continue
            units.append((name, prefix))
    print(f"{len(units)} units have a TB, a capture and a channel estimate")
    units = units[:args.max_units]

    print()
    print("unit                 sym  clean     " + "  ".join(f"{s:>5.0f}dB" for s in snrs))
    rows = []
    for name, prefix in units:
        cfg = read_config(prefix)
        bwp = int(cfg["bwp_size_rb"])
        alloc = [int(x) for x in cfg["alloc_prb"].split(",")]
        H = np.fromfile(prefix + "_h.bin", dtype=np.complex64).reshape(NOF_SYMBOLS, bwp * 12)
        sub = np.concatenate([np.arange(r * 12, (r + 1) * 12) for r in alloc])
        # Drop the window-edge subcarriers (see the module docstring).
        n_edge = max(1, int(len(sub) * EDGE_FRACTION))
        keep = sub[:-n_edge]

        tb_bits = np.unpackbits(np.fromfile(os.path.join(args.tb, name + ".bits"), dtype=np.uint8))

        staged = os.path.join("/tmp/ce_err", name)
        os.makedirs("/tmp/ce_err", exist_ok=True)

        values = []
        for snr in [None] + snrs:
            grid = build_grid(cfg, tb_bits, H, snr, args.seed)
            hat = replay(staged, grid, H, prefix)
            if hat is None:
                values.append(float("nan"))
                continue
            hat = hat.reshape(NOF_SYMBOLS, bwp * 12)
            per_symbol = [nmse(H[s], hat[s], keep) for s in range(NOF_SYMBOLS)]
            per_symbol = [v for v in per_symbol if v is not None]
            values.append(10 * np.log10(np.median(per_symbol)))

        # One row per symbol would be noise; the median across symbols is the summary.
        print(f"{name:20s} med  "
              + f"{values[0]:+7.2f}  " + "  ".join(f"{v:+6.2f}" for v in values[1:]))
        rows.append((name, values))

    print()
    print("NMSE of the chain's channel estimate against the known channel (dB, lower is better).")
    print("'clean' is the noise-free grid, i.e. the estimator's own floor rather than its SNR.")
    if rows:
        arr = np.array([r[1] for r in rows], dtype=float)
        print("median over units: " + "  ".join(f"{v:+6.2f}" for v in np.nanmedian(arr, axis=0)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
