#!/usr/bin/env python3
"""Evaluate a trained refiner against the chain, with one NMSE definition and no shortcuts.

The point of a separate script is that the metric gets written once. While this work was being
done the same quantity was computed ad hoc several times and gave +2.44, -22.98 and -17.57 dB on
the same pair, because each version got one of these wrong:

  * the axis order ([n, 2, symbol, subcarrier] as stored by `load`, versus the [subcarrier,
    symbol] order the dataset is stored in);
  * the direction of the gain fit (`g` must map the ESTIMATE onto the TARGET);
  * the normalisation of the residual (by the fitted reference, or by the true power).

So the rule here is: build the complex pair explicitly, fit the gain explicitly, and print the
intermediate quantities so a wrong number is visible rather than merely plausible.

Usage
-----
    eval_refiner.py --model <best.keras> --data <npz> [--per-unit]
"""

import argparse
import os
import sys

os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "3")
import numpy as np  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import train_refiner as T  # noqa: E402


def complex_pair(block, width):
    """[2, symbol, subcarrier] real/imag -> [subcarrier, symbol] complex, first `width` columns.

    The two real channels are the FIRST axis in `load`'s layout, not the last. Getting that wrong
    silently mixes real and imaginary parts and produces a large, plausible-looking error.
    """
    real = block[0][:, :width]
    imag = block[1][:, :width]
    return real + 1j * imag


def nmse_db(estimate, target):
    """Scale-invariant NMSE in dB, with the best scalar gain mapping estimate onto target.

    `np.vdot(a, b)` conjugates its first argument, so the projection coefficient is
    `np.vdot(estimate, target) / np.vdot(estimate, estimate)`. Reversing the arguments keeps the
    gain's magnitude but flips its phase, which is invisible in a magnitude check and wrong by a
    large factor in the result. The orthogonality assertion below catches it.
    """
    estimate = estimate.ravel()
    target = target.ravel()
    gain = np.vdot(estimate, target) / (np.vdot(estimate, estimate) + 1e-30)
    residual = target - gain * estimate
    reference = gain * estimate
    orthogonality = abs(np.vdot(residual, estimate)) / (
        np.sqrt(np.sum(np.abs(residual) ** 2) * np.sum(np.abs(estimate) ** 2)) + 1e-30)
    assert orthogonality < 1e-3, f"gain is not the least-squares optimum ({orthogonality:.3e})"
    return 10.0 * np.log10(np.sum(np.abs(residual) ** 2)
                           / (np.sum(np.abs(reference) ** 2) + 1e-30))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--model", required=True)
    parser.add_argument("--data", required=True)
    parser.add_argument("--meta", help="the dataset's _meta.csv, to attribute results to units")
    parser.add_argument("--per-unit", action="store_true")
    args = parser.parse_args()

    import tensorflow as tf
    import train_refiner as TR

    x, y, snr, widths = T.load(args.data)
    model = tf.keras.models.load_model(args.model,
                                       custom_objects={"Scale": TR.Scale})
    prediction = model.predict(x, batch_size=32, verbose=0)

    # Sanity: the identity predictor must reproduce the chain, and its NMSE must match the
    # dataset's own self-check. If this line disagrees with `build_synth_dataset --selfcheck`,
    # the layout is wrong and nothing below is worth reading.
    probe = 0
    width = int(widths[probe])
    est = complex_pair(x[probe], width)
    tgt = complex_pair(y[probe], width)
    gain = np.vdot(est.ravel(), tgt.ravel()) / (np.vdot(est.ravel(), est.ravel()) + 1e-30)
    print(f"sanity on sample {probe} (width {width}):")
    print(f"  |estimate| {np.abs(est).mean():.5f}   |target| {np.abs(tgt).mean():.5f}   "
          f"ratio {np.abs(est).mean() / np.abs(tgt).mean():.4f}")
    print(f"  best gain |g| {abs(gain):.4f}  phase {np.angle(gain):+.4f} rad")
    print(f"  residual/reference power "
          f"{np.sum(np.abs(tgt - gain * est) ** 2) / np.sum(np.abs(gain * est) ** 2):.5f}")
    print(f"  identity NMSE here: {nmse_db(est, tgt):+.2f} dB "
          f"(must match `build_synth_dataset --selfcheck`)")

    base = np.array([nmse_db(complex_pair(x[k], int(widths[k])),
                             complex_pair(y[k], int(widths[k]))) for k in range(len(x))])
    net = np.array([nmse_db(complex_pair(prediction[k], int(widths[k])),
                            complex_pair(y[k], int(widths[k]))) for k in range(len(x))])

    print()
    print("  SNR        chain      network     improvement")
    for level in sorted(set(snr)):
        sel = snr == level
        chain_med = np.median(base[sel])
        net_med = np.median(net[sel])
        print(f"  {level:5.1f} dB  {chain_med:+8.2f}  {net_med:+8.2f}  "
              f"{chain_med - net_med:+7.2f} dB   (n={int(sel.sum())})")
    print(f"  {'overall':>8}  {np.median(base):+8.2f}  {np.median(net):+8.2f}  "
          f"{np.median(base) - np.median(net):+7.2f} dB   (medians of all pairs)")
    print(f"  {'power':>8}  {10 * np.log10(np.mean(10 ** (base / 10))):+8.2f}  "
          f"{10 * np.log10(np.mean(10 ** (net / 10))):+8.2f}  "
          f"{10 * np.log10(np.mean(10 ** (base / 10)) / np.mean(10 ** (net / 10))):+7.2f} dB"
          f"   (aggregate power ratio)")

    improvement = base - net
    print()
    print(f"  improvement distribution: p5 {np.percentile(improvement, 5):+.2f}, "
          f"p25 {np.percentile(improvement, 25):+.2f}, p50 {np.median(improvement):+.2f}, "
          f"p75 {np.percentile(improvement, 75):+.2f}, p95 {np.percentile(improvement, 95):+.2f} dB")
    print(f"  pairs improved: {100 * np.mean(improvement > 0):.1f}%")

    if args.per_unit and args.meta:
        names = [line.split(",")[0] for line in open(args.meta).readlines()[1:] if line.strip()]
        if len(names) == len(base):
            by_unit = {}
            for k, name in enumerate(names):
                by_unit.setdefault(name, []).append(improvement[k])
            medians = np.array([np.median(v) for v in by_unit.values()])
            print(f"  units: {len(medians)}, median improvement {np.median(medians):+.2f} dB, "
                  f"improved {100 * np.mean(medians > 0):.1f}%")
        else:
            print(f"  (meta has {len(names)} rows but the dataset has {len(base)} pairs)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
