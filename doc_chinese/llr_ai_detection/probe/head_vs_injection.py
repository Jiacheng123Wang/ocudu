#!/usr/bin/env python3
"""E1/E2: does the published noise track an INJECTED noise, and what does the pipeline leave behind?

WHY THIS EXISTS. R7 measured the reporting heads' error against the noise the synthetic builder INJECTED, and
read 1.8-2.5 dB in the mid band and 2.2-2.7 dB at the top. Before blaming the heads, two things have to be
separated:

  E1  the synthetic pipeline's own residual. `--source synthetic` fits a channel to a real capture and
      regenerates the grid at a prescribed SNR. If the fitted channel is not the channel the pilots see, the
      generated grid carries more noise than the sigma2 it claims -- and then the head is being scored against
      a truth that the data does not contain.
  E2  the head's SLOPE. The unknown residual is common to every SNR level of the same unit, so regressing the
      published noise on the injected noise across levels cancels it: the slope is unbiased even when the
      intercept is not.

What is reported, per unit (a unit is one capture, generated at several SNRs):
    slope      published = slope * injected + intercept   (both in the plane's r^2 units)
    intercept  the noise the pipeline leaves when nothing is injected -- E1's answer
    floor_dB   10*log10(1/intercept), i.e. the best SINR this unit's data can honestly represent
    per level  the injected SNR, the published noise in dB, and the error in dB

A slope near 1 with a large intercept means the model is fine and the pipeline is the limit; a slope far from 1
means the head compresses and that is a head problem; both far from 1 means look at the input plane.

Usage:
  head_vs_injection.py --npz <file.npz> [--model <keras>] [--label NAME]
"""
import argparse
import os
import sys

import numpy as np

AI = os.environ.get("DEPTH3_AI_TRAIN",
                    "/Users/jiachengwang/dev/ocudu/lib/phy/upper/ai_signal_processors/depth3/ai_train")
sys.path.insert(0, AI)

import tensorflow as tf  # noqa: E402

tf.get_logger().setLevel("ERROR")
import depth3 as D3  # noqa: E402
import depth3_loss as L  # noqa: E402
import train_depth3 as T  # noqa: E402

W = "/Users/jiachengwang/dev/ocudu/doc_chinese/llr_ai_detection/work_tmp"
MODULATIONS = {2: "QPSK", 4: "16QAM", 6: "64QAM", 8: "256QAM"}


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--npz", required=True)
    parser.add_argument("--model", default=f"{W}/data/runs/d3_r7_c32/depth3_best.keras")
    parser.add_argument("--label", default="")
    args = parser.parse_args()

    data = np.load(args.npz, allow_pickle=True)
    qm = int(np.asarray(data["qm"])[0])
    modulation = MODULATIONS[qm]
    model = tf.keras.models.load_model(args.model, compile=False,
                                       custom_objects={"FoldedScaleConv": T.FoldedScaleConv})
    batches = T.Batches(args.npz, modulation, 0, layout="spatial", cond=True)

    units = np.asarray(data["unit"]) if "unit" in data.files else None
    rows = []
    for index in range(batches.n):
        sample = batches.sample(index)
        out = model(tf.constant(sample["x"][None, ...]), training=False)
        u = float(np.asarray(out["dmrs_noise"]).reshape(-1)[0])
        published = float(L.noise_from_shape(u))
        injected = float(np.asarray(data["sigma2_over_r2"])[index])
        prescribed = float(np.asarray(data["snr_nominal"])[index])
        # The builder names the unit "<capture>@<snr>", so the capture is the key: the residual this script
        # measures is per CAPTURE, and one level per key would make the fit impossible.
        unit = str(units[index]).split("@")[0] if units is not None else "?"
        rows.append((unit, prescribed, injected, published))

    label = args.label or os.path.basename(args.npz)
    print(f"{label}: {len(rows)} sample(s), model {os.path.basename(args.model)}, modulation {modulation}")
    print(f"  {'unit':>14} {'prescribed':>10} {'injected':>10} {'published dB':>12} {'err dB':>8}")
    by_unit = {}
    for unit, prescribed, injected, published in sorted(rows, key=lambda r: (r[0], -r[1])):
        err = 10.0 * np.log10(published / injected)
        by_unit.setdefault(unit, []).append((prescribed, injected, published, err))
        print(f"  {unit:>14} {prescribed:>9.1f} {injected:>10.3e} {10 * np.log10(published):>12.2f} {err:>+7.2f}")

    print()
    print(f"  {'unit':>14} {'slope':>7} {'intercept':>11} {'floor dB':>9} {'rms err dB':>11} {'worst err dB':>13}")
    for unit, samples in sorted(by_unit.items()):
        if len(samples) < 3:
            continue
        injected = np.array([s[1] for s in samples])
        published = np.array([s[2] for s in samples])
        slope, intercept = np.polyfit(injected, published, 1)
        floor_db = 10.0 * np.log10(1.0 / intercept) if intercept > 0 else float("nan")
        errs = np.array([s[3] for s in samples])
        print(f"  {unit:>14} {slope:>7.3f} {intercept:>11.3e} {floor_db:>9.1f} "
              f"{np.sqrt(np.mean(errs ** 2)):>11.2f} {np.max(np.abs(errs)):>13.2f}")

    print()
    print("  slope near 1 and a small intercept: the head tracks the injection and the pipeline is clean.")
    print("  a large intercept (low floor_dB) is E1's answer: the data cannot represent a cleaner channel, so")
    print("  the error measured at the high end is partly the builder's, not the head's.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
