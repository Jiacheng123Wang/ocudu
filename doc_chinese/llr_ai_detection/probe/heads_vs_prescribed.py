#!/usr/bin/env python3
"""The reporting heads against a PRESCRIBED SNR ladder: the "known truth" route of R6's accuracy criterion.

Why this and not only the natural validation set: on the validation set the true noise is whatever the channel
happened to have, so an error curve there mixes the head's own error with the sample mix. The synthetic dataset
prescribes the SNR -- 0, 6, 12, 18, 24, 30 dB on the six units of `one_sample/synth.npz` -- and carries the truth it
built each unit from (`sigma2_over_r2`, `H`), so both heads can be read against a KNOWN input rather than a selection.

What is compared, per unit:
    head 4   published noise (through the contract's model-external map) against the true noise, in dB
    head 3   NMSE against the true channel over the allocation, plus the power ratio (a shrink shows here)
The arm's own convention: the published noise is `10^(a*x+b)` from the noise head's tanh output, so the reported
"error dB" is 10*log10(published / true), the same quantity the design document's section 19.133 table carries.

Usage:
    heads_vs_prescribed.py [--model <keras>] [--data <npz>] [--modulation 256QAM]
"""
import argparse
import sys

import numpy as np

AI = "/Users/jiachengwang/dev/ocudu/lib/phy/upper/signal_processors/channel_estimator/metal/ai_train"
W = "/Users/jiachengwang/dev/ocudu/doc_chinese/llr_ai_detection/work_tmp"
sys.path.insert(0, AI)

import tensorflow as tf  # noqa: E402

tf.get_logger().setLevel("ERROR")
import depth3 as D3  # noqa: E402
import depth3_loss as L  # noqa: E402
import train_depth3 as T  # noqa: E402


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", default=f"{W}/data/runs/d3_modcond_ft/depth3_best.keras")
    parser.add_argument("--data", default=f"{W}/data/one_sample/synth.npz")
    parser.add_argument("--modulation", default="256QAM")
    parser.add_argument("--emit-sinr-cal", action="store_true",
                        help="fit the per-modulation affine IN THE ENGINE'S SPACE and print the --sinr-cal "
                             "string for train_depth3.py. The table below reports NOISE in dB, while the knob "
                             "multiplies the PUBLISHED SINR -- which is the same quantity negated, so a fit made "
                             "on the printed column needs its OFFSET NEGATED before it is declared. Measured "
                             "2026-10-18: declaring the noise-space offset as-is raised a reception published at "
                             "17.07 dB to 20.83 where it belonged at 18.20, the scheduler believed it, the "
                             "signalling was granted 64QAM, and attach failed on every leg that carried the "
                             "declaration. This option exists so nobody converts by hand again.")
    args = parser.parse_args()

    model = tf.keras.models.load_model(args.model, compile=False,
                                       custom_objects={"FoldedScaleConv": T.FoldedScaleConv})
    data = T.Batches(args.data, args.modulation, 0, layout="spatial", cond=True)
    print(f"{args.model.split('/')[-2]}, {data.n} prescribed unit(s), modulation {args.modulation}")
    print(f"{'prescribed':>10} {'head4 u':>9} {'published':>10} {'true':>9} {'err dB':>8}   "
          f"{'head3 NMSE':>11} {'power':>7}")

    spans_u, spans_t, errors = [], [], []
    cal_rows = []
    for index in range(data.n):
        sample = data.sample(index)
        out = model(tf.constant(sample["x"][None, ...]), training=False)
        # ---- head 4: one scalar, through the contract's model-external map.
        u = float(np.asarray(out["dmrs_noise"]).reshape(-1)[0])
        published = float(L.noise_from_shape(u))
        true = float(data.data["sigma2_over_r2"][index])
        spans_u.append(u)
        spans_t.append(10.0 * np.log10(true))
        errors.append(10.0 * np.log10(published / true))
        cal_rows.append((float(data.data["snr_nominal"][index]), 10.0 * np.log10(published),
                         10.0 * np.log10(true)))
        # ---- head 3: the channel, over the allocation, with the sample's own weights (the training convention).
        channel = np.asarray(out["ch_est"]).reshape(D3.NOF_SUBCARRIERS, 2 * T.NOF_SYMBOLS)
        estimate = channel[:, 0::2] + 1j * channel[:, 1::2]
        truth = sample["ch"][:, :, 0] + 1j * sample["ch"][:, :, 1]
        weights = np.asarray(sample["ch_weights"]).reshape(estimate.shape)
        mask = weights > 0
        error_power = np.sum(np.abs(estimate - truth)[mask] ** 2)
        truth_power = np.sum(np.abs(truth)[mask] ** 2)
        nmse_db = 10.0 * np.log10(max(error_power, 1e-30) / max(truth_power, 1e-30))
        power_db = 10.0 * np.log10(max(np.sum(np.abs(estimate)[mask] ** 2), 1e-30) / max(truth_power, 1e-30))
        print(f"{float(data.data['snr_nominal'][index]):>9.1f} {u:>+9.3f} {10*np.log10(published):>9.2f} "
              f"{10*np.log10(true):>9.2f} {10*np.log10(published/true):>+8.2f}   {nmse_db:>10.2f} {power_db:>+7.2f}")

    print()
    print(f"head 4 output span {max(spans_u)-min(spans_u):.3f} over a true noise span of "
          f"{max(spans_t)-min(spans_t):.2f} dB; error range {min(errors):+.2f} .. {max(errors):+.2f} dB")
    inside = [e for e, snr in zip(errors, data.data["snr_nominal"]) if 12.0 <= float(snr) <= 30.0]
    if inside:
        print(f"inside the declared 12-30 dB working range: error {min(inside):+.2f} .. {max(inside):+.2f} dB "
              f"over {len(inside)} unit(s)")

    if args.emit_sinr_cal:
        # ---- the affine, fitted and reported in the space the ENGINE uses -------------------------------
        # FIT ONLY INSIDE THE DECLARED WORKING RANGE: the heads are documented to fail below ~12 dB (the 0 and
        # 6 dB points read +15 and +7 dB), and including them tilts the fit and hides the in-range residuals --
        # measured here at 5.53 dB worst inside 12-30 dB when the out-of-domain points were included against 0.52
        # when they were not.
        all_rows = sorted(cal_rows)
        rows = [r for r in all_rows if 12.0 <= r[0] <= 30.0]
        if len(rows) < 3:
            rows = all_rows
        if len(rows) < 3:
            print("\n--emit-sinr-cal needs at least three prescribed points")
            return 1
        skipped = len(all_rows) - len(rows)
        # The knob multiplies the published SINR, which is -10log10(noise): the negation of the column above.
        sinr = [(snr, -noise_raw, -noise_true) for snr, noise_raw, noise_true in rows]
        (s_lo, r_lo, t_lo), (s_hi, r_hi, t_hi) = sinr[0], sinr[-1]
        if abs(r_hi - r_lo) < 1e-6:
            print("\n--emit-sinr-cal: the published SINR does not move across the ladder; no affine exists")
            return 1
        slope = (t_hi - t_lo) / (r_hi - r_lo)
        offset = t_lo - slope * r_lo
        worst = 0.0
        if skipped:
            print(f"\n({skipped} point(s) outside the declared 12-30 dB working range are not used in the fit)")
        print(f"SINR-space fit on the two end points ({s_lo:g} and {s_hi:g} dB), held-out residuals:")
        for snr, r, t in sinr:
            e = slope * r + offset - t
            worst = max(worst, abs(e))
            print(f"  prescribed {snr:5.1f} dB: raw {r:6.2f} -> {slope * r + offset:6.2f} vs truth {t:6.2f}  "
                  f"({e:+.2f} dB)")
        qm = {"QPSK": 2, "16QAM": 4, "64QAM": 6, "256QAM": 8}[args.modulation]
        print(f"\nworst held-out |err| = {worst:.2f} dB")
        print(f"--sinr-cal \"{qm}:{slope:.4f},{offset:+.4f}\"")
    return 0


if __name__ == "__main__":
    sys.exit(main())
