#!/usr/bin/env python3
"""Diagnose whether a SNR bin is information-limited or still has headroom.

The question this answers
------------------------
The refiner improves the chain's channel estimate by +3.17 dB at 12 dB input SNR but only
+0.60 dB at 24 dB, and at 24 dB it still sits ~2.5 dB above the floor that a perfect estimator
would reach. Two very different explanations fit that, and they call for opposite responses:

  * INFORMATION LIMIT - the chain's estimate no longer determines the channel well enough for
    anything to recover the rest. Nothing more can be done from this input; the only way forward
    would be a different input.
  * CAPACITY or TRAINING LIMIT - the information is there and the network is not extracting it,
    which more capacity, more epochs or a re-weighted loss would fix.

The test is to compute, on the same samples, the best correction an UNCONSTRAINED function could
apply, and compare the network against it. Three references are used, in increasing strength:

  1. `chain`    - do nothing (the identity predictor).
  2. `network`  - the trained refiner.
  3. per-RE optimal complex gain
                - for each (symbol, subcarrier) independently, the gain that minimises the error
                  on THAT sample. This is not a legitimate estimator (it uses the target), but it
                  is the strongest correction expressible as a per-RE gain, so it upper-bounds any
                  gain-only network. If the network is at or below it, no gain-based refinement
                  can do better.
  4. `oracle fit` - a smooth (polynomial-in-subcarrier) correction fitted to the residual, which
                  additionally captures the channel's frequency structure.

A network that sits clearly above (3) is leaving something that even a trivial per-RE gain could
have extracted, which points at capacity or training. A network at (3) is limited by what a
per-RE gain can express, so the remaining error is in the *structure* of the estimate.

Usage
-----
    diagnose_bin.py --model <best.keras> --data <npz> --snr 24 [--degree 4]
"""

import argparse
import os
import sys

os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "3")
import numpy as np  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)


def scale_invariant_nmse(estimate, target):
    """NMSE in dB after removing the best single complex gain (per sample)."""
    estimate = np.asarray(estimate).ravel()
    target = np.asarray(target).ravel()
    gain = np.vdot(estimate, target) / (np.vdot(estimate, estimate) + 1e-30)
    residual = target - gain * estimate
    return 10.0 * np.log10(np.sum(np.abs(residual) ** 2)
                           / (np.sum(np.abs(gain * estimate) ** 2) + 1e-30))


def per_re_optimal_gain(estimate, target):
    """Best per-(symbol, subcarrier) complex gain, evaluated with the same normalisation.

    This uses the target, so it is a bound rather than an estimator: it answers "how much of the
    remaining error is expressible as a per-RE gain at all".
    """
    gain = np.sum(estimate * np.conj(target), axis=0, keepdims=True) / (
        np.sum(np.abs(estimate) ** 2, axis=0, keepdims=True) + 1e-30)
    return gain * estimate


def smooth_correction(estimate, target, degree):
    """Add a polynomial-in-subcarrier correction to the estimate, fitted to the residual.

    Captures the part of the error that varies smoothly across frequency (a residual timing or
    phase ramp, for instance), which a per-RE gain cannot express.
    """
    residual = target - estimate
    n_sub, n_sym = estimate.shape
    x = np.linspace(-1.0, 1.0, n_sub)
    basis = np.stack([x ** d for d in range(degree + 1)], axis=1)      # [n_sub, degree+1]
    corrected = estimate.copy()
    for s in range(n_sym):
        coefficients, _, _, _ = np.linalg.lstsq(basis, residual[:, s], rcond=None)
        corrected[:, s] = estimate[:, s] + basis @ coefficients
    return corrected


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--model", required=True)
    parser.add_argument("--data", required=True)
    parser.add_argument("--snr", type=float, required=True)
    parser.add_argument("--degree", type=int, default=4,
                        help="degree of the polynomial correction used as the 'oracle fit'")
    args = parser.parse_args()

    import tensorflow as tf
    import train_refiner as T

    x, y, snr, widths = T.load(args.data)
    model = tf.keras.models.load_model(args.model, custom_objects={"Scale": T.Scale})
    prediction = model.predict(x, batch_size=32, verbose=0)

    sel = np.where(snr == args.snr)[0]
    print(f"SNR {args.snr} dB: {len(sel)} samples")

    def cx(block, k):
        n = int(widths[k])
        return block[k, 0][:, :n] + 1j * block[k, 1][:, :n]

    rows = {"chain": [], "network": [], "per-RE gain": [], "oracle fit": []}
    for k in sel:
        target = cx(y, k)
        chain = cx(x, k)
        net = cx(prediction, k)
        rows["chain"].append(scale_invariant_nmse(chain, target))
        rows["network"].append(scale_invariant_nmse(net, target))
        rows["per-RE gain"].append(scale_invariant_nmse(per_re_optimal_gain(chain, target), target))
        rows["oracle fit"].append(
            scale_invariant_nmse(smooth_correction(chain, target, args.degree), target))

    print()
    print("  predictor                                  NMSE (median)   vs chain")
    base = np.median(rows["chain"])
    for label in ("chain", "network", "per-RE gain", "oracle fit"):
        value = np.median(rows[label])
        print(f"  {label:40s}  {value:+8.2f} dB   {base - value:+6.2f} dB")

    print()
    net_v = np.median(rows["network"])
    gain_v = np.median(rows["per-RE gain"])
    print(f"  network minus per-RE-gain bound: {net_v - gain_v:+.2f} dB")
    if net_v <= gain_v + 0.3:
        print("  => The network is AT the per-RE-gain bound: the remaining error is not")
        print("     expressible as a per-RE gain, so a gain-based refinement cannot improve it.")
    else:
        print("  => The network is ABOVE the per-RE-gain bound, i.e. a trivial per-RE gain would")
        print("     have extracted more than the network did. That points at capacity or training")
        print("     rather than at an information limit.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
