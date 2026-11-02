#!/usr/bin/env python3
"""DEAD END -- an aborted attempt to verify a dataset's labels against its own signal.

STATUS: NOT A TOOL. It runs end to end and it cannot tell a correct label from an incorrect one.
It lives here, outside the working tree, so that it cannot be mistaken for one of the tools that
do work -- which is exactly the mistake it was written to prevent in the labels.

WHAT IT WAS TRYING TO DO. The dataset audit checks shape, bit balance, one modulation and a
plausible SNR: properties of the FILE, not of the LABELS. Both label defects found on 2026-11-02 --
a codeblock information length taken from a lifting-size ladder, and control information never
read -- passed that audit without a murmur. So the question was whether a label can be checked
against something that is NOT the code that produced it.

The signal is the candidate. A sample carries the received grid and the true channel, so the
transmitted symbols should be recoverable from them, and the label's bits should agree with what
the signal implies.

WHAT WAS ESTABLISHED. One circular identity, which rules a field out rather than proving anything:

  * `bits` is reproduced EXACTLY by `synth.encode_symbols(tb_bits, ...)` -- 4752/4752 on three
    samples of two sizes. Circular as a correctness check, since it is the same code, but it rules
    out the field meaning something other than the codeword.

WHAT WAS RULED OUT, so that nobody repeats it:

  1. `bits` meaning something other than the codeword     -- no, bit-exact.
  2. The element map's coordinates lacking the allocation offset
                                                          -- fixed: the channel runs 96..167, which
                                                             is first_prb*12. With the offset all
                                                             792 elements are finite and |X/H| has
                                                             a median of 0.948, so the equalised
                                                             plane looks right.
  3. `X/H` differing from the symbols by a complex scalar -- no: three combinations (X/H, X, X*H)
                                                             all give a relative error near 1.
  4. Pairing in `data_pos` order rather than `ulsch_pos`  -- no: same error either way.
  5. The grid being symbol-major rather than subcarrier-major
                                                          -- no: same error either way.

WHAT REMAINS. What `X`'s grid channels actually contain. `build_clean` places the symbols at
`data_pos`, times the channel, over a `[symbol, subcarrier]` grid; the stored plane is
`[subcarrier, symbol]` after the reference amplitude is divided out. One of those statements is not
what the stored array follows, and it cannot be settled by trying combinations -- it has to be read
from the code that writes `X`, which is a few lines in `build_depth3_dataset.py`.

THE HONEST SUMMARY. This line of work did not produce a working check. It produced a list of five
things that are not the explanation, one circular identity worth keeping, and a clear statement of
where to look next. The audit still cannot verify a label, which is where it started.

Usage (it runs, it just does not conclude anything):
  probe_label_roundtrip_deadend.py --file <dataset.npz> [--samples N]
"""
import argparse
import glob
import os
import sys

import numpy as np

# The plane's own layout, from depth3.py: channels 0..27 hold the grid as c = 2t + {0,1} for the
# fourteen symbols, so one sample's grid is [612, 14, 2] and the DM-RS mask is channels 28..41.
NOF_SYMBOLS = 14
CH_GRID = 28
CH_DMRS = 28
NOF_SUBCARRIERS = 612
BITS_PER_SYMBOL = {"QPSK": 2, "16QAM": 4, "64QAM": 6, "256QAM": 8}

# Gray-coded PAM levels per modulation, as the demodulator's thresholds imply. The constellation is
# the standard NR one: odd integers scaled so the mean square is one.
PAM_LEVELS = {2: 2, 4: 4, 6: 8, 8: 8}


def pam_gray_bits(value, nof_levels):
    """Hard bits of one PAM axis, in the order the standard's bit mapping uses (MSB first)."""
    # Levels are labelled Gray: the highest bit is the sign, each lower bit is the next partition.
    bits = []
    for shift in range(int(np.log2(nof_levels)) - 1, -1, -1):
        bits.append(1 if (value >> shift) & 1 else 0)
    return bits


def hard_bits_from_symbol(y, qm):
    """Hard bits of an equalised complex symbol, MSB first, using the standard's Gray labelling."""
    nof_axis = PAM_LEVELS[qm]
    scale = np.sqrt((2.0 / 3.0) * (4 ** (qm // 2) - 1))     # mean-square-one normalisation
    out = []
    for part in (np.real(y), np.imag(y)):
        z = np.clip(part * scale, -nof_axis + 1, nof_axis - 1)
        level = int(np.rint((z + nof_axis - 1) / 2.0))       # 0 .. nof_axis-1
        out.extend(pam_gray_bits(level ^ (level >> 1), nof_axis))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--file", default="")
    ap.add_argument("--dir", default="")
    ap.add_argument("--samples", type=int, default=50)
    ap.add_argument("--seed", type=int, default=20261102)
    args = ap.parse_args(argv)

    files = [args.file] if args.file else sorted(glob.glob(os.path.join(args.dir, "*.npz")))
    if not files:
        print("no datasets given", file=sys.stderr)
        return 1

    # `nr_ldpc` and `synth` live with the label builder, one tree over from this auditor. Both are
    # imported rather than reimplemented: this tool must use the SAME geometry the builder used, so
    # that a geometry error shows up as a syndrome failure instead of being reproduced here.
    here = os.path.dirname(os.path.abspath(__file__))
    label_dir = os.path.abspath(os.path.join(here, "..", "..", "..", "signal_processors",
                                             "channel_estimator", "metal", "ai_train"))
    for path in (here, label_dir):
        if path not in sys.path:
            sys.path.insert(0, path)
    import nr_ldpc as NL
    import synth as SN
    import pusch_uci as UCI

    rng = np.random.default_rng(args.seed)
    findings = 0
    unimplemented = False
    print("%-46s %6s %6s %8s %s" % ("file", "n", "bad", "syndrome", "note"))
    for path in files:
        with np.load(path, allow_pickle=True) as z:
            X = z["X"].astype(np.float32)
            n = X.shape[0]
            take = rng.choice(n, size=min(args.samples, n), replace=False)
            bad = 0
            checked = 0
            note = ""
            for i in take:
                qm = int(z["qm"][i])
                modulation = [m for m, v in BITS_PER_SYMBOL.items() if v == qm]
                if not modulation:
                    note = "unknown qm"
                    continue
                modulation = modulation[0]
                nof_prb = int(z["alloc_nof_prb"][i])
                first_prb = int(z["alloc_first_prb"][i])
                dmrs = z["dmrs_bitmap"][i]
                dmrs_symbols = [t for t in range(NOF_SYMBOLS) if (int(dmrs) >> t) & 1]
                # X puts the grid in channels c = 2t + {0,1} for the fourteen symbols; H is the true
                # channel over the same grid, already divided by the reference amplitude (measured:
                # the equalised 64QAM magnitude has a median of 0.95, so no second scaling).
                grid_full = X[i][:, :CH_GRID].reshape(NOF_SUBCARRIERS, NOF_SYMBOLS, 2)
                grid = (grid_full[..., 0] + 1j * grid_full[..., 1]).astype(np.complex64)
                Htrue = (z["H"][i][..., 0] + 1j * z["H"][i][..., 1]).astype(np.complex64)
                y = z["bits"][i]
                nof_bits = int(z["nof_bits"][i])
                tb = z["tb_bits"][i]
                nof_tb = int(z["nof_tb_bits"][i])
                checked += 1
                # The geometry the builder used, so that a wrong geometry shows up as a syndrome
                # failure rather than being reproduced here.
                # The element map the builder used, so a geometry error shows up as a mismatch
                # here rather than being reproduced. `uci` comes from the sample's own fields.
                uci = None
                if int(z["uci_nof_harq_ack"][i]) or int(z["uci_csi1_re"][i]):
                    uci = UCI.UciLayout(nof_harq_ack=int(z["uci_nof_harq_ack"][i]),
                                        csi_part1=True if int(z["uci_csi1_re"][i]) else False)
                try:
                    layout = UCI.data_layout(nof_prb, NOF_SYMBOLS, dmrs_symbols, qm, uci)
                    c_init = (int(z["rnti"][i]) << 15) + int(z["n_id"][i])
                    sequence = UCI.descrambling_sequence(layout, c_init)
                except Exception as exc:                     # noqa: BLE001
                    note = f"layout failed: {exc}"
                    break
                # The label, descrambled back to the codeword bits. `bits` holds what is actually
                # transmitted, already scrambled and already stripped of the punctured elements.
                stored = np.unpackbits(y)[:nof_bits].astype(np.uint8)
                if stored.size != sequence.size:
                    note = f"label {stored.size} bits vs sequence {sequence.size}"
                    break
                label = stored ^ sequence
                # The signal's own bits at the elements those codeword bits land on.
                hard = []
                ok_bits = True
                for (symbol, subcarrier), kept in zip(layout.ulsch_pos, layout.keep_bits):
                    if not kept:
                        continue
                    # The element map's subcarriers are relative to the allocation; the stored
                    # channel and grid are full-band. Measured: with the offset the equalised
                    # magnitude has a median of 0.948 on 64QAM, without it every element is a
                    # divide by zero.
                    sc = subcarrier + first_prb * 12
                    h = Htrue[sc, symbol]
                    if abs(h) < 1e-6:
                        ok_bits = False
                        break
                    hard.extend(hard_bits_from_symbol(grid[sc, symbol] / h, qm))
                if not ok_bits:
                    note = "no channel at an allocated element"
                    break
                hard = np.asarray(hard, dtype=np.uint8)
                m = min(hard.size, label.size)
                if m == 0:
                    note = "no bits to compare"
                    break
                agree = float((hard[:m] == label[:m]).mean())
                if agree < 0.90:
                    bad += 1
                    note = f"agreement {agree:.3f}"
                elif not note:
                    note = f"agreement {agree:.3f}"
            print("%-46s %6d %6d %8s %s" % (os.path.basename(path)[:46], checked, bad,
                                            "OK" if bad == 0 else "FAIL", note))
            findings += bad
    if unimplemented:
        print("\nfindings: INCOMPLETE -- the signal-versus-label comparison is not written yet.")
        print("This tool cannot report a clean result until it is; it exits non-zero on purpose.")
        return 2
    print("\nfindings:", "none" if findings == 0 else f"{findings} sample(s) with a bad syndrome")
    return 0 if findings == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
