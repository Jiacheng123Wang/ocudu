#!/usr/bin/env python3
"""R4.5: fit the model-external LLR scale, and measure what the quantiser costs.

The contract keeps every scale OUTSIDE the graph (`model_network_design.md` section 0.2(f)) for two
reasons: changing one must not mean retraining, and the quantiser's cost has to be measurable on its
own. This is that measurement.

For each candidate `S_llr` the validation captures are decoded twice through the real NR LDPC code --
once with the QUANTISED LLRs (`log_likelihood_ratio::quantize`, values in [-120, 120]) and once with
the same LLRs before rounding -- and both coded BLERs are reported. The design's acceptance is that
the quantised and unquantised outcomes differ by no more than the pre-registered threshold; a scale
whose quantised decode is WORSE than its unquantised one is paying for the quantiser, and one whose
quantised decode is BETTER is doing the opposite, which is the signature of a head that is
under-confident and gets help from a coarser grid.

The map is the contract's, and it has exactly one implementation (`depth3.llr_from_logits`), so an
offline fit and the receiver cannot disagree about what a scale means.

Usage:
    calibrate_depth3.py --model <run>/depth3_best.keras --data <val.npz> \
                        [--scales 2.5,5,10,20,40,80,160] [--samples 96] [--write-scales <dir>]
"""

import argparse
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import tensorflow as tf          # noqa: E402

import depth3 as D3              # noqa: E402
import depth3_loss as L          # noqa: E402
import train_depth3 as T         # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--model", required=True)
    parser.add_argument("--data", required=True)
    parser.add_argument("--modulation", default="256QAM")
    parser.add_argument("--layout", choices=("folded", "spatial", "probe8"), default="spatial")
    parser.add_argument("--samples", type=int, default=96)
    parser.add_argument("--scales", default="2.5,5,10,20,40,80,160")
    parser.add_argument("--current", type=float, default=20.0,
                        help="the value the model was TRAINED with, which is also the receiver's "
                             "default. The sweep is flat over a range, and a calibration that moved "
                             "the value inside that range would change a trained convention for no "
                             "measured gain")
    parser.add_argument("--write-scales", default="",
                        help="directory to write depth3_scales.txt into, for the receiver to read")
    args = parser.parse_args()

    tf.get_logger().setLevel("ERROR")
    data = T.Batches(args.data, args.modulation, max_samples=args.samples, layout=args.layout,
                     seed=20261013)
    model = tf.keras.models.load_model(args.model, compile=False,
                                       custom_objects={"FoldedScaleConv": T.FoldedScaleConv})
    print(f"{os.path.basename(args.model)} on {data.n} samples of {os.path.basename(args.data)} "
          f"({args.modulation}, layout {args.layout})")

    # One forward pass per sample, cached: the sweep must not pay for the network again per scale,
    # and every scale must see exactly the same head output.
    heads = []
    for index in data.order:
        index = int(index)
        sample = data.sample(index)
        outputs = model(tf.constant(sample["x"][None, ...]), training=False)
        plane = np.asarray(outputs["llr"]).reshape(D3.NOF_SUBCARRIERS, -1)
        heads.append((index, plane))
    print(f"cached {len(heads)} head outputs, shape {heads[0][1].shape}")

    print(f"\n{'S_llr':>8} {'uBER':>8} {'|LLR|':>8} {'saturated':>10} "
          f"{'BLER unquantised':>17} {'BLER quantised':>15} {'blocks':>8}")
    results = []
    for scale in [float(value) for value in args.scales.split(",")]:
        bler_unquantised = T.CodedBler(data)
        bler_quantised = T.CodedBler(data)
        errors = 0
        total = 0
        magnitude = 0.0
        saturated = 0
        for index, plane in heads:
            alloc = data.allocation_of(index)
            dmrs_symbols = data.dmrs_symbols_of(index)
            modulation = data.modulation_of(index)
            shape = D3.llr_gather(plane, alloc, dmrs_symbols, modulation)
            unquantised = shape * scale
            quantised = D3.llr_from_logits(shape, modulation, scale).astype(np.float32)
            bler_unquantised.add(index, unquantised)
            bler_quantised.add(index, quantised)

            sample = data.sample(index)
            labels = D3.llr_gather(sample["llr_shape"], alloc, dmrs_symbols, modulation) > 0.5
            quantised_labels = quantised > 0
            errors += int((quantised_labels != labels).sum())
            total += int(labels.size)
            magnitude += float(np.abs(quantised).sum())
            saturated += int((np.abs(quantised) >= D3.LLR_MAX).sum())
        results.append((scale, errors / max(total, 1), magnitude / max(total, 1),
                        100.0 * saturated / max(total, 1), bler_unquantised.rate(),
                        bler_quantised.rate(), bler_quantised.blocks))
        row = results[-1]
        print(f"{row[0]:8.2f} {row[1]:8.4f} {row[2]:8.1f} {row[3]:9.2f}% "
              f"{row[4]:17.4f} {row[5]:15.4f} {row[6]:8d}")

    # The fit is a PLATEAU, not a point, and the unquantised column proves why: normalised min-sum is
    # invariant to a common scale, so every scale decodes the same and only the quantiser has an
    # opinion. The plateau is therefore the set of scales whose quantised decode equals the best
    # quantised decode, and the rule is to KEEP the value the model was trained with when it lies on
    # the plateau -- moving inside a flat region changes a trained convention and buys nothing.
    #
    # "Equals" has to mean "equals to the resolution of this measurement", and the resolution is ONE
    # BLOCK: a BLER is a count of failed blocks over `blocks`, so two scales whose counts differ by one
    # are the same measurement. An exact-equality plateau turns that one block into a refit, and it did
    # -- on 616 blocks it moved the deployed scale from the trained 20 to 10 on a single block (0.3929
    # against 0.3945), which is a change to a trained convention bought with noise.
    best_bler  = min(row[5] for row in results)
    blocks     = max(row[6] for row in results)
    resolution = 1.0 / max(blocks, 1)
    plateau = [row[0] for row in results if row[5] - best_bler <= resolution + 1.0e-12]
    on_plateau = any(abs(scale - args.current) < 1.0e-9 for scale in plateau)
    chosen = args.current if on_plateau else min((row for row in results if row[5] == best_bler),
                                                 key=lambda row: row[0])[0]
    print(f"\nplateau (quantised BLER {best_bler:.4f}, resolution {resolution:.4f} = one of {blocks} "
          f"blocks): S_llr in [{min(plateau):g}, {max(plateau):g}]")
    print(f"the model was trained with S_llr = {args.current:g}, "
          f"{'which is on the plateau -- KEPT' if on_plateau else 'which is NOT on it -- refitted'}")
    print(f"chosen S_llr = {chosen:g}")
    best = next(row for row in results if abs(row[0] - chosen) < 1.0e-9)
    print(f"quantised BLER {best[5]:.4f} against {best[4]:.4f} unquantised "
          f"(difference {best[5] - best[4]:+.4f})")

    if args.write_scales:
        path = os.path.join(args.write_scales, "depth3_scales.txt")
        with open(path, "w") as handle:
            handle.write(f"# R4.5 fit from {os.path.basename(args.model)} on "
                         f"{os.path.basename(args.data)}, {len(heads)} samples\n")
            handle.write(f"s_llr={best[0]:g}\n")
            # The noise pair travels with the LLR scale, and it is not a fit: heads 2 and 4 were
            # TRAINED against `depth3_loss.noise_shape_target`, so the map the receiver has to invert
            # is fixed by the training, and the trainer's constants are the only correct source for it.
            # Written here because the receiver otherwise falls back to its own defaults, which happen
            # to be the same numbers today and are two independent literals in two languages -- a
            # change to either side would move the published noise by decades with no symptom until
            # the scheduler read a wrong SINR.
            handle.write(f"a_noise={L.NOISE_SCALE_DECADES:g}\n")
            handle.write(f"b_noise={L.NOISE_SCALE_CENTER:g}\n")
        print(f"wrote {path} (s_llr={best[0]:g}, a_noise={L.NOISE_SCALE_DECADES:g}, "
              f"b_noise={L.NOISE_SCALE_CENTER:g})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
