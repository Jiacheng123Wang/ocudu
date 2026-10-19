#!/usr/bin/env python3
"""What the two REPORTING heads actually support (R6's gate before anything is published).

Heads 3 and 4 are only as good as their training, and stage 2 gives them a small weight (`--w-ch`,
`--w-noise`) because the LLR head is the one the receiver is judged on. Before the chain publishes a
measurement computed from them, the question is not "does the code path run" -- it runs, and the chain
prints the numbers -- but "how far are they from the truth they are supposed to report".

Three comparisons, all on the validation set, all in dB:

* head 3 against the true channel, over the allocation and at the pilot resource elements. The dataset's
  `H` is already `H/r`, which is what the head produces, so the two are directly comparable and the
  reference amplitude cannot hide a factor.
* head 3's POWER against the true channel's: a mean-square-error fit that shrinks towards zero (which is
  what an MMSE estimator does) shows up here as a negative bias, and a bias in power is exactly what a
  scheduler would read as channel quality.
* head 4 through the contract's map against the sample's own `sigma2_over_r2`.

Usage:
    check_reporting_heads.py --model <run>/depth3_best.keras --data <npz> [--samples 64]
"""

# The shared DSP utilities (dmrs, synth, labels, LDPC) still live in the AI-CE training tree; this is
# the single place that knows where, so the import below is not a hidden path dependency.
import depth3_paths
depth3_paths.add_ce_shared()
import argparse
import os
import sys

import numpy as np

# A sibling of the training pipeline it measures -- the same directory as `train_depth3.py`, `depth3.py` and
# `measure_ce_error.py` -- so the import needs no path arithmetic: it used to live under the (git-ignored)
# `work_tmp/probe`, and a tool that judges the model has to be versioned with the model's code.
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import depth3 as D3              # noqa: E402
import depth3_loss as L          # noqa: E402
import train_depth3 as T         # noqa: E402

NOF_SYMBOLS = 14


def dB(value):
    return 10.0 * np.log10(max(float(value), 1.0e-30))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--model", required=True)
    parser.add_argument("--data", required=True)
    parser.add_argument("--modulation", default="256QAM")
    parser.add_argument("--layout", choices=("folded", "spatial", "probe8"), default="spatial")
    parser.add_argument("--samples", type=int, default=64)
    parser.add_argument("--per-modulation", type=int, default=0,
                        help="take this many samples from EACH modulation instead of the first --samples. A "
                             "conditioned dataset is stored modulation by modulation, so a head taken from the "
                             "front of it is a QPSK-only measurement that looks like a whole-model one -- the "
                             "mistake eval_modcond.py made on the same file.")
    parser.add_argument("--cond", action="store_true",
                        help="the model is MODULATION-CONDITIONED: the samples carry their own Qm, the loader "
                             "appends the one-hot channels, and the results are broken down per modulation. "
                             "A conditioned and a per-modulation bundle have identical reporting heads, so the "
                             "only way to judge one is to feed it the contract it was trained under.")
    args = parser.parse_args()

    import tensorflow as tf
    tf.get_logger().setLevel("ERROR")

    data = T.Batches(args.data, args.modulation,
                     max_samples=0 if args.per_modulation else args.samples, layout=args.layout,
                     cond=args.cond)
    model = tf.keras.models.load_model(args.model, compile=False,
                                       custom_objects={"FoldedScaleConv": T.FoldedScaleConv})

    # WHICH samples. Balanced when asked for: the archive is concatenated per modulation, so "the first N" is a
    # single-modulation measurement however the summary prints it.
    if args.per_modulation:
        qm_of = data.data["qm"]
        indices = []
        for name in sorted(D3.BITS_PER_SYMBOL, key=lambda value: D3.BITS_PER_SYMBOL[value]):
            wanted = D3.BITS_PER_SYMBOL[name]
            picked = [i for i in range(qm_of.size) if int(qm_of[i]) == wanted][:args.per_modulation]
            if not picked:
                print(f"the dataset carries no {name} sample")
            indices += picked
    else:
        indices = list(range(data.n))
    print(f"{len(indices)} samples of {os.path.basename(args.data)} through "
          f"{os.path.basename(args.model)}")

    nmse_alloc = []
    nmse_pilot = []
    power_ratio = []
    noise_error = []
    # Per-modulation buckets, filled only under --cond. R6's gate is per modulation: a conditioned bundle that
    # reported well on three of them and badly on the fourth would pass an aggregate and fail the chain exactly
    # where the scheduler reads it.
    per_qm = {}
    for index in indices:
        sample = data.sample(index)
        outputs = model(tf.constant(sample["x"][None, ...]), training=False)

        # ---- head 3: the channel estimate, per resource element --------------------------------
        head = np.asarray(outputs["ch_est"]).reshape(D3.NOF_SUBCARRIERS, NOF_SYMBOLS, 2)
        estimate = (head[..., 0] + 1j * head[..., 1]).astype(np.complex64)
        truth = (sample["ch"][:, :, 0] + 1j * sample["ch"][:, :, 1]).astype(np.complex64)

        allocated = np.zeros(D3.NOF_SUBCARRIERS, dtype=bool)
        for prb in data.allocation_of(index):
            allocated[prb * 12:(prb + 1) * 12] = True
        pilots = np.zeros((D3.NOF_SUBCARRIERS, NOF_SYMBOLS), dtype=bool)
        pilots[np.ix_(np.flatnonzero(allocated), data.dmrs_symbols_of(index))] = True

        allocation_mask = np.repeat(allocated[:, None], NOF_SYMBOLS, axis=1)
        for mask, sink in ((allocation_mask, nmse_alloc), (pilots, nmse_pilot)):
            error = float((np.abs(estimate[mask] - truth[mask]) ** 2).sum())
            reference = float((np.abs(truth[mask]) ** 2).sum())
            sink.append(error / (reference + 1e-30))
        power_ratio.append(float((np.abs(estimate[allocation_mask]) ** 2).sum() /
                                 (np.abs(truth[allocation_mask]) ** 2).sum()))

        # ---- head 4: the DM-RS noise variance, through the contract's map ----------------------
        u = float(np.asarray(outputs["dmrs_noise"]).reshape(-1)[0])
        published = float(L.noise_from_shape(u))
        target = float(data.data["sigma2_over_r2"][index])
        noise_error.append(dB(published / target))

        if args.cond:
            bucket = per_qm.setdefault(data.modulation_of(index), [[], [], [], []])
            bucket[0].append(nmse_alloc[-1])
            bucket[1].append(nmse_pilot[-1])
            bucket[2].append(power_ratio[-1])
            bucket[3].append(noise_error[-1])

    if args.cond:
        print("\nper modulation (the conditioned contract's own breakdown):")
        print(f"{'modulation':<10} {'n':>4} {'NMSE alloc':>11} {'NMSE pilot':>11} "
              f"{'power ratio':>12} {'noise err':>10}")
        for name in sorted(per_qm, key=lambda value: D3.BITS_PER_SYMBOL[value]):
            rows = [np.asarray(column) for column in per_qm[name]]
            print(f"{name:<10} {rows[0].size:>4} {dB(np.median(rows[0])):>8.2f} dB "
                  f"{dB(np.median(rows[1])):>8.2f} dB {dB(np.median(rows[2])):>9.2f} dB "
                  f"{np.median(rows[3]):>7.2f} dB")

    nmse_alloc = np.asarray(nmse_alloc)
    nmse_pilot = np.asarray(nmse_pilot)
    power_ratio = np.asarray(power_ratio)
    noise_error = np.asarray(noise_error)

    print(f"\nhead 3 NMSE over the allocation : median {dB(np.median(nmse_alloc)):7.2f} dB  "
          f"(p10 {dB(np.percentile(nmse_alloc, 10)):6.2f}, p90 {dB(np.percentile(nmse_alloc, 90)):6.2f})")
    print(f"head 3 NMSE at the pilots       : median {dB(np.median(nmse_pilot)):7.2f} dB")
    print(f"head 3 power ratio (est/true)   : median {dB(np.median(power_ratio)):+7.2f} dB  "
          f"(negative = the estimate is SHRUNK towards zero)")
    print(f"head 4 published/true           : median {np.median(noise_error):+7.2f} dB  "
          f"(p10 {np.percentile(noise_error, 10):+6.2f}, p90 {np.percentile(noise_error, 90):+6.2f})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
