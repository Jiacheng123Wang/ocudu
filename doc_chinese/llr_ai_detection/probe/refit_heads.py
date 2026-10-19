#!/usr/bin/env python3
"""Refit the four reporting heads of a TRUNCATED trunk, so a depth ablation measures capability, not mismatch.

The problem this solves (measured 2026-10-17): truncating the deployed model from 18 blocks to 15 drops head 3's
NMSE from -27.9 dB to -13.7 dB. That is not "three blocks of capability": the heads were trained on block 18's
features, so feeding them block 15's is feeding them a distribution they never saw. A depth-versus-capability curve
taken without refitting the heads measures the mismatch and nothing else.

So: freeze the truncated trunk and train ONLY the twelve head layers, with the same loss, weights and per-sample
scale the deployed model's fine-tune used. The trunk keeps exactly the weights it was trained with, the head is the
trained head continued on the new features, and what is left of the gap is the depth.

    Cost: a few hundred steps against a frozen trunk -- minutes, not the hours a from-scratch run costs, and no
    trunk weight changes. This is a fair ablation, NOT a compressed model being trained.

Subset note: the merged dataset is ORDERED BY MODULATION, so `max_samples` would take QPSK alone (the eval script
records the same trap). Indices are therefore spread evenly over the whole file.
"""
import argparse
import os
import subprocess
import sys

import numpy as np

AI = "/Users/jiachengwang/dev/ocudu/lib/phy/upper/ai_signal_processors/depth3/ai_train"
W = "/Users/jiachengwang/dev/ocudu/doc_chinese/llr_ai_detection/work_tmp"
sys.path.insert(0, AI)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import tensorflow as tf  # noqa: E402

tf.get_logger().setLevel("ERROR")
import depth3 as D3  # noqa: E402
import depth3_loss as L  # noqa: E402
import train_depth3 as T  # noqa: E402
from truncate_depth import truncate  # noqa: E402

HEAD_LAYERS = {"llr_1x1", "llr", "llr_flat", "noise_1x1", "post_eq_noise", "noise_flat",
               "ch_est_1x1", "ch_est", "ch_est_flat", "dmrs_pool", "dmrs_noise_1x1", "dmrs_noise"}


def stack(samples, key):
    return np.stack([np.asarray(sample[key]) for sample in samples]).astype(np.float32)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--checkpoint", default=f"{W}/data/runs/d3_modcond_ft/depth3_best.keras")
    parser.add_argument("--depth", type=int, required=True)
    parser.add_argument("--steps", type=int, default=400)
    parser.add_argument("--batch", type=int, default=8)
    parser.add_argument("--lr", type=float, default=1e-3)
    parser.add_argument("--samples", type=int, default=1600,
                        help="training samples, spread across all four modulations")
    parser.add_argument("--w-noise", type=float, default=0.3, help="the deployed fine-tune's noise weight")
    parser.add_argument("--snr-range", default="12.00,48.00")
    parser.add_argument("--train", default=f"{W}/data/datasets/d3_modcond_train.npz")
    args = parser.parse_args()

    base = tf.keras.models.load_model(args.checkpoint, compile=False,
                                      custom_objects={"FoldedScaleConv": T.FoldedScaleConv})
    model = truncate(base, args.depth)
    # The trunk is frozen by name and not by layer index, so a layer added to the head list above cannot quietly
    # leave the trunk trainable (which would turn this ablation into a training run at the reduced depth).
    for layer in model.layers:
        layer.trainable = layer.name in HEAD_LAYERS
    trainable = model.trainable_variables
    print(f"depth {args.depth}: {len(trainable)} head tensor(s) trainable, "
          f"{len(model.variables) - len(trainable)} trunk tensor(s) frozen")

    data = T.Batches(args.train, "256QAM", 0, seed=7, layout="spatial", cond=True)
    picks = np.linspace(0, data.n - 1, args.samples).astype(int)
    mods = {}
    for index in picks:
        mods[data.modulation_of(int(index))] = mods.get(data.modulation_of(int(index)), 0) + 1
    print(f"subset {len(picks)} sample(s) across {mods}")

    head_channels = T.NOF_SYMBOLS * D3.COND_STRIDE
    weights = {"llr": L.DEFAULT_WEIGHTS["llr"], "ch_est": L.DEFAULT_WEIGHTS["ch_est"],
               "post_eq_noise": args.w_noise, "dmrs_noise": args.w_noise}
    optimizer = tf.keras.optimizers.Adam(args.lr)

    @tf.function
    def step(x, targets):
        with tf.GradientTape() as tape:
            outputs = model(x, training=True)
            llr = (tf.reshape(tf.cast(outputs["llr"], tf.float32), (-1, D3.NOF_SUBCARRIERS, head_channels))
                   * targets["scale"][:, None, None])
            terms = {"llr": L.tf_bce_loss(llr, targets["llr_shape"], targets["llr_weights"])}
            ch = tf.reshape(tf.cast(outputs["ch_est"], tf.float32), (-1, D3.NOF_SUBCARRIERS, 2 * T.NOF_SYMBOLS))
            terms["ch_est"] = L.tf_nmse(tf.complex(ch[:, :, 0::2], ch[:, :, 1::2]),
                                        tf.complex(targets["ch"][:, :, :, 0], targets["ch"][:, :, :, 1]),
                                        targets["ch_weights"])
            noise = tf.reshape(tf.cast(outputs["post_eq_noise"], tf.float32),
                               (-1, D3.NOF_SUBCARRIERS, T.NOF_SYMBOLS))
            error = (noise - targets["noise"]) ** 2
            terms["post_eq_noise"] = (tf.reduce_sum(error * targets["noise_weights"])
                                      / tf.maximum(tf.reduce_sum(targets["noise_weights"]), 1.0))
            dmrs = tf.reshape(tf.cast(outputs["dmrs_noise"], tf.float32), (-1,))
            terms["dmrs_noise"] = tf.reduce_mean((dmrs - targets["dmrs_noise"]) ** 2)
            total = sum(weights[name] * terms[name] for name in terms)
        gradients = tape.gradient(total, trainable)
        optimizer.apply_gradients(zip(gradients, trainable))
        return total

    rng = np.random.default_rng(11)
    for iteration in range(args.steps):
        batch = rng.choice(picks, args.batch, replace=False)
        samples = [data.sample(int(index)) for index in batch]
        targets = {key: tf.constant(stack(samples, key)) for key in
                   ("llr_shape", "llr_weights", "ch", "ch_weights", "noise", "noise_weights",
                    "dmrs_noise", "scale")}
        loss = float(step(tf.constant(stack(samples, "x")), targets))
        if (iteration % 50 == 0) or (iteration == args.steps - 1):
            print(f"  step {iteration:4d}  loss {loss:.5f}", flush=True)

    tag = f"refit_b{args.depth}"
    keras_path = os.path.join(W, "data/models", f"{tag}.keras")
    package = os.path.join(W, "data/models", f"{tag}.mlpackage")
    model.save(keras_path)
    subprocess.run([sys.executable, os.path.join(AI, "train_depth3.py"), "--export-only", keras_path,
                    "--export-probe", package, "--modcond", "--layout", "spatial",
                    "--modulation", "256QAM", "--snr-range", args.snr_range], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(["xcrun", "coremlcompiler", "compile", package, os.path.join(W, "data/models")], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print(f"wrote {keras_path} and {tag}.mlmodelc")
    return 0


if __name__ == "__main__":
    sys.exit(main())
