#!/usr/bin/env python3
"""Train the depth-3 channel-refinement network on the synthetic dataset.

What this experiment is for
---------------------------
It answers a feasibility question with a measurable answer: can a network whose input is the
chain's own channel estimate get close to what a PERFECT estimate would give? The ceiling is
known from `measure_oracle_gain.py`, and it is about 1.2 dB at the 20 dB operating point, so
"close to the ceiling" means "within of order a tenth of a dB", not "better by a mile".

The target is the channel the grid was built from, so `input - target` is the chain's real
estimation error rather than a second noisy estimate of it (that distinction is what rules out
the real label corpus; see sections 35.5 and 37.4 of the design record).

Loss
----
Scale-invariant NMSE: the best single COMPLEX gain is removed before measuring the residual.
This is deliberate, not a convenience. The chain applies a scale convention of its own
(measured at 1/1.43 relative to the true channel), a demapper estimates a gain anyway, and a
constant scale carries no information - so making the network reproduce an arbitrary constant
would spend capacity on nothing and would make the reported NMSE depend on that constant.

Layout
------
The window is [14 symbols, 624 subcarriers]; 612 is the 51 PRB width the network was sized for
and the remaining 12 columns are padding that a masked loss ignores. The network is the v1
topology: 18 blocks of (3x3 depthwise -> 1x1 pointwise -> ReLU) with channel 64 and dilations
cycling 1,2,4,8, then a 1x1 head to two real channels. It is fed in the NCHW layout the CoreML
export uses, so training and deployment see the same tensor.

Usage
-----
    train_refiner.py --train <npz> --val <npz> --out <dir> [--epochs 40] [--blocks 18]
"""

import argparse
import os
import sys

os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "3")
import numpy as np  # noqa: E402

import tensorflow as tf  # noqa: E402

tf.get_logger().setLevel("ERROR")

NOF_SYMBOLS = 14
WIDTH = 624                      # 612 used + 12 padding columns
USED_SUBCARRIERS = 612
C_HID = 64


@tf.keras.utils.register_keras_serializable(package="ocudu")
class Scale(tf.keras.layers.Layer):
    """A per-channel multiply, initialised small (LayerScale).

    It exists so a residual block starts as a near-identity: with a plain `y = block(y)` stack
    the activations collapse geometrically with depth (measured on this graph: 0.044 -> 0.005 ->
    0.0003 -> 0 over the first four blocks), every gradient past the collapse is exactly zero,
    and training stalls at "no better than predicting nothing". BatchNorm would also fix the
    collapse but costs much more on the ANE, and the whole point of this network is that it runs
    there.
    """

    def __init__(self, initial=0.1, **kwargs):
        super().__init__(**kwargs)
        self.initial = initial

    def get_config(self):
        return {**super().get_config(), "initial": self.initial}

    def build(self, input_shape):
        self.scale = self.add_weight(name="scale", shape=(input_shape[-1],),
                                     initializer=tf.keras.initializers.Constant(self.initial),
                                     trainable=True)

    def call(self, inputs):
        return inputs * self.scale


def build(blocks=18, c_hid=C_HID, width=WIDTH, symbols=NOF_SYMBOLS):
    """The v1 graph: dilated depthwise/pointwise residual blocks, NCHW in and out.

    Dilations cycle 1,2,4,8 so the receptive field grows without spending FLOPs - dilation was
    measured to be free (section 27) - and 18 blocks put the theoretical receptive field at 127
    subcarriers, inside the channel's measured frequency coherence length of 150-300.

    Shape note: the input is [1, 2, symbol, subcarrier] and is transposed ONCE to channels-last,
    because a Keras conv is channels-last and the CoreML export feeds this layout directly.
    """
    dilations = [1, 2, 4, 8]
    inp = tf.keras.Input(shape=(2, symbols, width), batch_size=None, name="x")
    y = tf.keras.layers.Permute((2, 3, 1), name="to_nhwc")(inp)
    for i in range(blocks):
        block = tf.keras.layers.DepthwiseConv2D(
            3, padding="same", dilation_rate=dilations[i % len(dilations)],
            activation="relu", name=f"dw{i}_3x3")(y)
        block = tf.keras.layers.Conv2D(c_hid, 1, activation="relu", name=f"pw{i}_1x1")(block)
        block = Scale(name=f"scale{i}")(block)
        # A 1x1 projection on the skip only when the channel count changes; after the first block
        # the width already matches, so the rest is a plain add.
        if y.shape[-1] != c_hid:
            y = tf.keras.layers.Conv2D(c_hid, 1, name=f"skip{i}_1x1")(y)
        y = tf.keras.layers.Add(name=f"add{i}")([y, block])
    y = tf.keras.layers.Conv2D(2, 1, name="head_1x1")(y)
    # The head predicts a RESIDUAL on top of the input. Without this the network starts from
    # random weights and begins by destroying the signal it was given, and it has to spend
    # training rediscovering the identity map before it can improve on it (measured: the loss sat
    # at ~1.0, "no better than predicting nothing", while the identity it needed scores 0.0125).
    y = tf.keras.layers.Add(name="residual_add")([y, tf.keras.layers.Permute((2, 3, 1))(inp)])
    return tf.keras.Model(inp, tf.keras.layers.Permute((3, 1, 2), name="to_nchw")(y))


def complex_tensor(t):
    """[..., (re, im)] -> [...,] complex64.

    The cast must happen BEFORE multiplying by 1j: `float_tensor * 1j` raises, because TF will
    not promote a Python complex literal against a float tensor.
    """
    real = tf.cast(t[..., 0], tf.complex64)
    imag = tf.cast(t[..., 1], tf.complex64)
    return real + tf.cast(1j, tf.complex64) * imag


def scale_invariant_nmse(y_true, y_pred):
    """NMSE after removing the best single complex gain, computed per batch element.

    `y` is [batch, 2, symbols, width] with the two real channels interleaved as (re, im). Both
    arguments arrive as real tensors from Keras, so the complex form is rebuilt here.
    """
    true = tf.transpose(y_true, [0, 2, 3, 1])
    pred = tf.transpose(y_pred, [0, 2, 3, 1])
    true_c = complex_tensor(true)
    pred_c = complex_tensor(pred)

    # The gain is a global least-squares fit per sample, which is exactly the constant a demapper
    # would estimate from the data.
    numerator = tf.reduce_sum(true_c * tf.math.conj(pred_c), axis=[1, 2])
    denominator = tf.reduce_sum(tf.abs(pred_c) ** 2, axis=[1, 2]) + 1e-12
    gain = numerator / tf.cast(denominator, tf.complex64)
    residual = true_c - gain[:, None, None] * pred_c
    reference = gain[:, None, None] * pred_c
    nmse = (tf.reduce_sum(tf.abs(residual) ** 2, axis=[1, 2])
            / (tf.reduce_sum(tf.abs(reference) ** 2, axis=[1, 2]) + 1e-12))
    return tf.reduce_mean(nmse)


def masked_mse(y_true, y_pred, mask):
    """Plain MSE over the valid subcarriers of the padded window.

    The mask is passed explicitly rather than derived, because deriving it from "where the target
    is non-zero" lets the network score a free perfect result over the zero padding; with 12 of
    624 columns padded that was enough for the loss to fall while the real accuracy got worse.

    It is a plain MSE and not a scale-invariant one on purpose: `load` scales both sides by the
    target's per-sample RMS, so the arbitrary scale convention between the chain's estimate and
    the true channel is already removed, and the loss then measures shape only.
    """
    squared = tf.square(y_true - y_pred) * mask[:, None, None, :]
    count = tf.reduce_sum(mask) * tf.cast(tf.shape(y_true)[1] * tf.shape(y_true)[2], tf.float32)
    return tf.reduce_sum(squared) / tf.maximum(count, 1.0)


def load(path, width=WIDTH):
    """Load a dataset as [n, 2, symbol, subcarrier], padded to `width`.

    The padding columns are zero in both tensors, which is what `masked_nmse` keys on: it derives
    the validity mask from where the target is non-zero, so the padded region is excluded from the
    loss and from every reported NMSE automatically. The dataset itself stops at 612 (51 PRB),
    while the graph is 624 wide, and the extra 12 columns are what keeps a 3x3 kernel from
    treating the band edge as a boundary.
    """
    data = np.load(path)
    x = np.transpose(data["X"], (0, 3, 2, 1)).astype(np.float32)   # [n,2,symbol,subcarrier]
    y = np.transpose(data["Y"], (0, 3, 2, 1)).astype(np.float32)
    widths = data["width"]

    # Normalise BOTH sides by the target's own scale, per sample, over the valid width.
    #
    # This is what makes a plain MSE the right loss. The chain applies a scale convention of its
    # own - measured at 1/1.43 relative to the true channel - and a demapper estimates a gain
    # anyway, so the absolute scale carries no information. Without this normalisation a plain
    # MSE spends the whole budget fitting that arbitrary constant: measured, the identity
    # predictor scores +0.18 dB under a plain MSE while the scale-free NMSE of the same pair is
    # -19.14 dB. Dividing both sides by the same per-sample reference removes the ambiguity
    # without changing what the network has to learn.
    reference = np.sqrt(np.mean(y ** 2, axis=(1, 3), keepdims=True) + 1e-30)
    x = x / reference
    y = y / reference

    if x.shape[3] < width:
        pad = [(0, 0), (0, 0), (0, 0), (0, width - x.shape[3])]
        x = np.pad(x, pad)
        y = np.pad(y, pad)
    return x, y, data["snr_train"], widths


def nmse_db(x, y, width):
    """Reference: the scale-invariant NMSE of `x` against `y`, restricted to `width`."""
    a = (y[:, 0] + 1j * y[:, 1])[:, :, :width]
    b = (x[:, 0] + 1j * x[:, 1])[:, :, :width]
    af = a.reshape(len(a), -1)
    bf = b.reshape(len(b), -1)
    gain = np.sum(af * np.conj(bf), axis=1) / (np.sum(np.abs(bf) ** 2, axis=1) + 1e-12)
    residual = af - gain[:, None] * bf
    return 10 * np.log10(np.sum(np.abs(residual) ** 2, axis=1)
                         / np.sum(np.abs(gain[:, None] * bf) ** 2, axis=1))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--train", required=True)
    parser.add_argument("--val", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--epochs", type=int, default=40)
    parser.add_argument("--batch-size", type=int, default=16)
    parser.add_argument("--lr", type=float, default=1e-3)
    parser.add_argument("--blocks", type=int, default=18)
    parser.add_argument("--seed", type=int, default=20261010)
    args = parser.parse_args()

    tf.keras.utils.set_random_seed(args.seed)
    os.makedirs(args.out, exist_ok=True)

    x_train, y_train, snr_train, widths_train = load(args.train)
    x_val, y_val, snr_val, widths_val = load(args.val)
    print(f"train {x_train.shape}  val {x_val.shape}")

    # The baseline the network has to beat. Its input IS the chain's estimate, so the identity
    # predictor is exactly the chain, and this number is the chain's own error.
    widths_train = widths_train.astype(np.int32)
    base_train = nmse_db(x_train, y_train, USED_SUBCARRIERS)
    print()
    # The identity predictor IS the chain, so this is the chain's own error, per SNR, measured
    # with the same scale-invariant metric used for the network and the oracle.
    base_val = np.array([nmse_db(x_val[k:k + 1], y_val[k:k + 1], int(widths_val[k]))[0]
                         for k in range(len(x_val))])
    print("baseline (identity = the chain's own estimate), scale-invariant NMSE:")
    print(f"  train median {np.median(base_train):+7.2f} dB")
    for snr in sorted(set(snr_val)):
        sel = snr_val == snr
        print(f"  val @ {snr:4.1f} dB  median {np.median(base_val[sel]):+7.2f} dB  "
              f"(n={int(sel.sum())})")

    model = build(args.blocks)
    # `width` comes from the dataset, not from the padding: see MaskedNMSE.
    print(f"loss: plain MSE over each sample's own width "
          f"({int(widths_train.min())}..{int(widths_train.max())} subcarriers); the scale "
          f"convention is removed at evaluation time instead")
    optimizer = tf.keras.optimizers.Adam(args.lr)
    model.summary(expand_nested=False)
    mask_train = np.stack([np.concatenate([np.ones(int(w)), np.zeros(WIDTH - int(w))])
                           for w in widths_train]).astype(np.float32)
    mask_val = np.stack([np.concatenate([np.ones(int(w)), np.zeros(WIDTH - int(w))])
                         for w in widths_val]).astype(np.float32)
    history = []
    best_val = float("inf")
    best_path = os.path.join(args.out, "best.keras")
    for epoch in range(args.epochs):
        order = np.random.permutation(len(x_train))
        train_losses = []
        for start in range(0, len(order), args.batch_size):
            batch = order[start:start + args.batch_size]
            with tf.GradientTape() as tape:
                prediction = model(x_train[batch], training=True)
                loss = masked_mse(y_train[batch], prediction, mask_train[batch])
            gradients = tape.gradient(loss, model.trainable_variables)
            optimizer.apply_gradients(zip(gradients, model.trainable_variables))
            train_losses.append(float(loss))

        # Validation loss uses the SAME loss and the validation mask.
        val_losses = []
        for start in range(0, len(x_val), args.batch_size):
            batch = slice(start, start + args.batch_size)
            prediction = model(x_val[batch], training=False)
            val_losses.append(float(masked_mse(y_val[batch], prediction, mask_val[batch])))
        train_loss = float(np.mean(train_losses))
        val_loss = float(np.mean(val_losses))
        history.append((epoch + 1, train_loss, val_loss))
        if val_loss < best_val:
            best_val = val_loss
            model.save(best_path)
        print(f"  epoch {epoch + 1:3d}/{args.epochs}  train {train_loss:.6f}  "
              f"val {val_loss:.6f}{'  *' if val_loss == best_val else ''}", flush=True)

    with open(os.path.join(args.out, "history.csv"), "w") as handle:
        handle.write("epoch,loss,val_loss\n")
        for row in history:
            handle.write(f"{row[0]},{row[1]:.7f},{row[2]:.7f}\n")

    # Report in the units that matter: the network against the identity baseline, per SNR, with
    # the same metric used to compare against the oracle ceiling.
    best = tf.keras.models.load_model(best_path, custom_objects={"Scale": Scale})
    pred = best.predict(x_val, batch_size=args.batch_size, verbose=0)
    net = np.array([nmse_db(pred[k:k + 1], y_val[k:k + 1], int(widths_val[k]))[0]
                    for k in range(len(pred))])
    print()
    print("  val SNR    identity (chain)   network      improvement")
    for snr in sorted(set(snr_val)):
        sel = snr_val == snr
        print(f"  {snr:6.1f} dB   {np.median(base_val[sel]):+8.2f} dB   "
              f"{np.median(net[sel]):+8.2f} dB   {np.median(base_val[sel] - net[sel]):+7.2f} dB")
    print()
    print(f"  overall (median): identity {np.median(base_val):+.2f} dB, "
          f"network {np.median(net):+.2f} dB, "
          f"improvement {np.median(base_val - net):+.2f} dB")
    print(f"  oracle ceiling at this operating point is about +1.2 dB "
          f"(measure_oracle_gain.py)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
