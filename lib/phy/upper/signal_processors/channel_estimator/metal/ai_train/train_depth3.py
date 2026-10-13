#!/usr/bin/env python3
"""Train the depth-3 network: the contract's plane in, LLRs and the reporting heads out.

What this trains, per `model_network_design.md` section 0.2 and `design` section 3.2:

* the main head is trained with per-bit BCE against the SCRAMBLED coded bits, whose labels are
  `1 - b` because the chain reads a positive LLR as bit 0. The logit IS the LLR, so nothing is
  converted at inference;
* the channel-estimate head is trained with NMSE against the channel divided by the reference
  amplitude, on the allocated resource elements only: everywhere else the contract does not
  constrain it;
* the post-equalization noise head is trained in `log10` against the chain's own zero-forcing
  form, and its targets outside the representable window are excluded rather than clipped
  (`depth3_loss.noise_shape_target`).

The inputs are built by the same functions the dataset builder and the seam use (`depth3`), so
the trainer cannot drift from the contract by having its own idea of the layout.

The revision-version note: the labels are the SCRAMBLED bits and therefore in the scrambling
domain, which is where the contract's LLRs live. Descrambling happens after this unit, as
`memo_01` section 3 records.

Usage
-----
    train_depth3.py --train <npz> --val <npz> --out <dir> [--epochs 150] [--max-train N]
                    [--blocks 18] [--modulation 256QAM] [--no-aux]
"""

import argparse
import json
import os
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import depth3 as D3                # noqa: E402
import depth3_loss as L            # noqa: E402
# The canonical LayerScale for this workstream lives next to the refiner that needed it first.
from train_refiner import Scale    # noqa: E402

NOF_SYMBOLS = D3.NOF_SYMBOLS
DILATIONS = (1, 2, 4, 8)


def build(width=D3.NOF_SUBCARRIERS, blocks=18, qm=8, aux_heads=True, c_hid=64, residual=True,
          dtype="float32"):
    """The contract's graph: the plane in, the reporting heads out.

    `residual` and `dtype` are the two lessons the refiner paid for (`model_network_design`
    sections 38.3 and 42):

    * a plain `y = block(y)` stack collapses geometrically with depth (0.044 -> 0.005 -> 0.0003
      over the first four blocks), every gradient past the collapse is exactly zero, and training
      stalls at "no better than predicting nothing". Measured again on THIS task by the smoke run
      in the design document's section 12.9: the loss fell to 1.277, the mean LLR to 1.1 and the
      uncoded BER stayed at 0.498, which is chance. A residual block with a LayerScale avoids the
      collapse, and BatchNorm would too but costs far more on the ANE;
    * training in fp16 stalls; the deployed artifact is fp16 either way, because the conversion
      chooses that, so the graph dtype here is a training decision only.
    """
    import tensorflow as tf

    keras_dtype = tf.float32 if dtype == "float32" else tf.float16
    inp = tf.keras.Input(shape=(width, 1, D3.NOF_CHANNELS), batch_size=None, dtype=keras_dtype,
                         name="x")
    y = inp
    for i in range(blocks):
        dilation = DILATIONS[i % len(DILATIONS)]
        block = tf.keras.layers.DepthwiseConv2D(3, padding="same", dilation_rate=(dilation, 1),
                                                dtype=keras_dtype, name=f"dw{i}_3x3_d{dilation}")(y)
        block = tf.keras.layers.ReLU(dtype=keras_dtype, name=f"relu{i}")(block)
        block = tf.keras.layers.Conv2D(c_hid, 1, dtype=keras_dtype, name=f"pw{i}_1x1")(block)
        block = tf.keras.layers.ReLU(dtype=keras_dtype, name=f"relu{i}b")(block)
        if residual:
            block = Scale(name=f"scale{i}", dtype=keras_dtype)(block)
            y = tf.keras.layers.Add(name=f"add{i}", dtype=keras_dtype)([y, block])
        else:
            y = block

    outputs = {"llr": tf.keras.layers.Activation(
        "tanh", dtype=keras_dtype, name="llr")(
            tf.keras.layers.Conv2D(NOF_SYMBOLS * qm, 1, dtype=keras_dtype, name="llr_1x1")(y))}
    if aux_heads:
        outputs["post_eq_noise"] = tf.keras.layers.Activation(
            "tanh", dtype=keras_dtype, name="post_eq_noise")(
                tf.keras.layers.Conv2D(NOF_SYMBOLS, 1, dtype=keras_dtype, name="noise_1x1")(y))
        # Linear for the channel: it must reach deep fades as well as peaks.
        outputs["ch_est"] = tf.keras.layers.Activation(
            "linear", dtype=keras_dtype, name="ch_est")(
                tf.keras.layers.Conv2D(2 * NOF_SYMBOLS, 1, dtype=keras_dtype, name="ch_est_1x1")(y))
    return tf.keras.Model(inp, outputs)


class Batches:
    """Builds training tensors from a dataset file, on the fly.

    The label planes are derived per sample from the stored coded bits and masks instead of being
    stored, so the file stays small and the mapping from bits to channels has exactly one
    implementation (`depth3.label_plane`).
    """

    ARRAYS = ("X", "H", "bits", "nof_bits", "alloc_first_prb", "alloc_nof_prb", "dmrs_bitmap",
              "qm", "sigma2_over_r2", "snr_nominal")

    def __init__(self, path, modulation, max_samples=0, seed=0):
        # Read every array once, eagerly. The file is a compressed archive, and accessing
        # `data["X"]` per sample would decompress the whole 200 MB array every time.
        archive = np.load(path, allow_pickle=True)
        self.data = {name: archive[name] for name in self.ARRAYS}
        self.modulation = modulation
        self.qm = D3.BITS_PER_SYMBOL[modulation]
        self.n = len(self.data["X"])
        if max_samples:
            self.n = min(self.n, max_samples)
        self.order = np.arange(self.n)
        self.rng = np.random.default_rng(seed)

    def sample(self, index):
        data = self.data
        first_prb = int(data["alloc_first_prb"][index])
        nof_prb = int(data["alloc_nof_prb"][index])
        alloc = list(range(first_prb, first_prb + nof_prb))
        bitmap = int(data["dmrs_bitmap"][index])
        dmrs_symbols = [s for s in range(NOF_SYMBOLS) if (bitmap >> s) & 1]
        modulation = {v: k for k, v in D3.BITS_PER_SYMBOL.items()}[int(data["qm"][index])]
        bits = np.unpackbits(data["bits"][index])[:int(data["nof_bits"][index])]

        plane = data["X"][index].astype(np.float32)
        label = D3.label_plane(bits, alloc, dmrs_symbols, modulation)
        llr_weights = D3.loss_weights(alloc, dmrs_symbols, modulation)

        channel = (data["H"][index][:, :, 0].astype(np.float32)
                   + 1j * data["H"][index][:, :, 1].astype(np.float32))
        # The channel head is only constrained where the allocation is, and the target is already
        # divided by the reference amplitude, which is what the head produces.
        subcarriers = np.flatnonzero(D3.allocated_subcarriers(alloc))
        ch_weights = np.zeros((D3.NOF_SUBCARRIERS, NOF_SYMBOLS), dtype=np.float32)
        ch_weights[subcarriers, :] = 1.0

        sigma2_norm = float(data["sigma2_over_r2"][index])
        with np.errstate(divide="ignore", invalid="ignore"):
            noise_target = L.noise_shape_target(sigma2_norm / (np.abs(channel) ** 2 + 1e-30))
        noise_weights = (ch_weights > 0) & noise_target[1]

        return {
            "x": plane[:, None, :],
            "llr_shape": label,
            "llr_weights": llr_weights,
            "ch": np.stack([channel.real, channel.imag], axis=-1),
            "ch_weights": ch_weights,
            "noise": noise_target[0],
            "noise_weights": noise_weights.astype(np.float32),
            "snr": float(data["snr_nominal"][index]),
        }

    def epoch(self, batch_size):
        """Yield batches for one epoch, reshuffling first."""
        self.rng.shuffle(self.order)
        for start in range(0, self.n, batch_size):
            indices = self.order[start:start + batch_size]
            yield [self.sample(int(i)) for i in indices]


def stack(samples, key):
    return np.stack([s[key] for s in samples]).astype(np.float32)


def decode_metrics(llr, labels, weights):
    """Uncoded bit error rate and mean |LLR| on the labelled entries.

    The uncoded rate is the honest early signal that the network learned the mapping: it tends to
    zero as the LLRs become right, and it sits at 0.5 while they are noise. Coded BLER through the
    real LDPC decoder is the acceptance criterion, and it needs the chain, not this metric.
    """
    hard = np.sign(llr)
    labelled = weights > 0
    errors = np.mean((hard[labelled] > 0) != (labels[labelled] > 0.5)) if labelled.any() else np.nan
    return float(errors), float(np.mean(np.abs(llr[labelled]))) if labelled.any() else np.nan


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--train", required=True)
    parser.add_argument("--val", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--modulation", default="256QAM")
    parser.add_argument("--blocks", type=int, default=18)
    parser.add_argument("--epochs", type=int, default=150)
    parser.add_argument("--batch", type=int, default=8)
    parser.add_argument("--lr", type=float, default=1e-3)
    parser.add_argument("--max-train", type=int, default=0, help="cap the training samples")
    parser.add_argument("--max-val", type=int, default=64)
    parser.add_argument("--w-llr", type=float, default=L.DEFAULT_WEIGHTS["llr"])
    parser.add_argument("--w-ch", type=float, default=L.DEFAULT_WEIGHTS["ch_est"])
    parser.add_argument("--w-noise", type=float, default=L.DEFAULT_WEIGHTS["post_eq_noise"])
    parser.add_argument("--no-aux", action="store_true", help="LLR head only, for the R2.4 ablation")
    parser.add_argument("--no-residual", action="store_true",
                        help="plain block stack. It collapses (section 12.9); kept so the "
                             "failure can be reproduced rather than remembered")
    parser.add_argument("--dtype", choices=("float32", "float16"), default="float32",
                        help="training dtype. The exported artifact is fp16 regardless")
    parser.add_argument("--seed", type=int, default=20261013)
    args = parser.parse_args()

    import tensorflow as tf
    tf.get_logger().setLevel("ERROR")
    tf.random.set_seed(args.seed)

    train = Batches(args.train, args.modulation, args.max_train, seed=args.seed)
    val = Batches(args.val, args.modulation, args.max_val, seed=args.seed + 1)
    qm = train.qm
    print(f"train {train.n} samples, val {val.n} samples, {args.modulation} (Qm={qm})")

    model = build(blocks=args.blocks, qm=qm, aux_heads=not args.no_aux,
                  residual=not args.no_residual, dtype=args.dtype)
    parameters = int(sum(np.prod(w.shape) for w in model.weights))
    print(f"parameters {parameters}")

    # The scale is model-external per the contract, so the loss sees the LLR the quantizer sees.
    scale = D3.RANGE_LIMIT_FLOAT[args.modulation]
    optimizer = tf.keras.optimizers.Adam(args.lr)
    weights = {"llr": args.w_llr, "ch_est": args.w_ch, "post_eq_noise": args.w_noise}

    @tf.function
    def step(x, targets, training):
        with tf.GradientTape() as tape:
            outputs = model(x, training=training)
            llr = tf.cast(outputs["llr"], tf.float32)[:, :, 0, :] * scale
            terms = {"llr": L.tf_bce_loss(llr, targets["llr_shape"], targets["llr_weights"])}
            if not args.no_aux:
                ch = tf.cast(outputs["ch_est"], tf.float32)[:, :, 0, :]
                terms["ch_est"] = L.tf_nmse(
                    tf.complex(ch[:, :, 0::2], ch[:, :, 1::2]),
                    tf.complex(targets["ch"][:, :, :, 0], targets["ch"][:, :, :, 1]),
                    targets["ch_weights"])
                noise = tf.cast(outputs["post_eq_noise"], tf.float32)[:, :, 0, :]
                error = (noise - targets["noise"]) ** 2
                terms["post_eq_noise"] = (tf.reduce_sum(error * targets["noise_weights"])
                                          / tf.maximum(tf.reduce_sum(targets["noise_weights"]), 1.0))
            total = sum(weights[name] * terms[name] for name in terms)
        if training:
            gradients = tape.gradient(total, model.trainable_variables)
            optimizer.apply_gradients(zip(gradients, model.trainable_variables))
        return total, terms, llr

    def tensors(samples):
        return (
            tf.constant(stack(samples, "x")),
            {
                "llr_shape": tf.constant(stack(samples, "llr_shape")),
                "llr_weights": tf.constant(stack(samples, "llr_weights")),
                "ch": tf.constant(stack(samples, "ch")),
                "ch_weights": tf.constant(stack(samples, "ch_weights")),
                "noise": tf.constant(stack(samples, "noise")),
                "noise_weights": tf.constant(stack(samples, "noise_weights")),
            },
        )

    def evaluate():
        totals, errors, magnitudes = [], [], []
        for samples in val.epoch(args.batch):
            x, targets = tensors(samples)
            total, _, llr = step(x, targets, training=False)
            ber, mag = decode_metrics(llr.numpy(), targets["llr_shape"].numpy(),
                                      targets["llr_weights"].numpy())
            totals.append(float(total))
            errors.append(ber)
            magnitudes.append(mag)
        return float(np.mean(totals)), float(np.mean(errors)), float(np.mean(magnitudes))

    os.makedirs(args.out, exist_ok=True)
    history = []
    best = np.inf
    started = time.time()
    for epoch in range(1, args.epochs + 1):
        losses = []
        for samples in train.epoch(args.batch):
            x, targets = tensors(samples)
            total, _, _ = step(x, targets, training=True)
            losses.append(float(total))
        val_loss, val_ber, val_mag = evaluate()
        history.append({"epoch": epoch, "train": float(np.mean(losses)), "val": val_loss,
                        "val_ber": val_ber, "val_llr_mean": val_mag})
        if epoch == 1 or epoch % max(1, args.epochs // 20) == 0 or epoch == args.epochs:
            print(f"epoch {epoch:4d}  train {np.mean(losses):.5f}  val {val_loss:.5f}  "
                  f"uncoded BER {val_ber:.4f}  mean|LLR| {val_mag:6.1f}  "
                  f"({time.time() - started:.0f}s)")
        if val_loss < best:
            best = val_loss
            model.save(os.path.join(args.out, "depth3_best.keras"))

    with open(os.path.join(args.out, "history.json"), "w") as handle:
        json.dump({"history": history, "parameters": parameters,
                   "weights": weights, "scale": scale, "modulation": args.modulation}, handle,
                  indent=1)
    np.save(os.path.join(args.out, "val_llr_stats.npy"),
            np.array([[h["val_ber"], h["val_llr_mean"]] for h in history]))
    print(f"best val loss {best:.5f}; wrote {args.out}/depth3_best.keras and history.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
