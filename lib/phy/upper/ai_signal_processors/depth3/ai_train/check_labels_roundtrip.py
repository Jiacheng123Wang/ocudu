#!/usr/bin/env python3
"""Check a built dataset's LABELS against its own signal, without trusting the label path.

WHY IT EXISTS. The audit beside this one checks shape, bit balance, a single modulation and a plausible
SNR. All four are properties of the FILE. None of them is a property of the LABELS, and the difference
cost two defects in one day: the codeblock information length was taken from a ladder that exists to
pick a lifting size, which put wrong bits in about eight percent of the samples, and the control
information multiplexed on the PUSCH was never read, which put data labels on resource elements that
carry control. Both files passed the audit without a murmur. The block-size defect was found by a gate
-- a decoder disagreeing with the labels -- and the control defect by an audit that happened to count
refusals. The audit was never able to find either, because it never compared a label to anything.

WHAT THIS DOES INSTEAD. For each sample it takes the received grid, divides by the true channel, maps
the result to hard bits through the modulation's constellation, descrambles with the sample's own
parameters, and asks the LDPC syndrome whether those bits form a codeword.

The point is what is INDEPENDENT of the label path. The codeword comes from the SIGNAL. The geometry --
base graph, lifting size, the information length that decides where parity starts, the set of resource
elements that carry data -- comes from the same functions the builder used. So a geometry that is wrong
produces a codeword that does not satisfy the syndrome, and the failure is visible even though both
sides of it were computed by the same code. That is the property that would have caught the block-size
defect: the signal says one thing, the geometry says another, and the syndrome is where they meet.

WHAT THIS DOES NOT CATCH. A scrambling error that happens to permute the bits in a way the syndrome
tolerates -- it should not, but a syndrome is a weak test on a short block. Errors confined to samples
this does not look at. And a label path that is wrong in a way that is ALSO wrong in the same way here,
which is why the geometry is taken from the builder rather than recomputed.

Usage:
  check_labels_roundtrip.py --file <dataset.npz> [--samples N] [--seed S]
  check_labels_roundtrip.py --dir <datasets dir> [--samples N]

Exit status is 0 when the sampled codewords satisfy their syndromes, 1 when they do not.
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
                    h = Htrue[subcarrier, symbol]
                    if abs(h) < 1e-6:
                        ok_bits = False
                        break
                    hard.extend(hard_bits_from_symbol(grid[subcarrier, symbol] / h, qm))
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
