#!/usr/bin/env python3
"""The depth-3 training objective.

Authority: `design` section 3.2 (losses) and `model_network_design.md` section 0.2 (the contract).
Two identities in here are load-bearing and are checked by `--selfcheck` rather than trusted:

* **The logit IS the LLR.** Binary cross entropy on the coded bits trains a logit whose optimum is
  `ln[P(bit=0)/P(bit=1)]`, so the trained quantity needs no probability-to-LLR conversion. That is
  why "output LLRs" and "train with BCE" are the same statement, and why a reference-LLR
  regression is not used as the main loss (`design` section 3.2, second point): regressing onto a
  teacher's LLRs distils the teacher's ceiling into the student.
* **Positive means bit 0** (`log_likelihood_ratio.h:22-25`). Therefore the label is `1 - b` and not
  `b`. An inverted label inverts every LLR, and the only symptom is that the model does not learn,
  so `--selfcheck` builds the logits of a perfect model from the chain's convention and asserts the
  loss is zero. With the label the wrong way round the same logits score `S_llr` per bit.

The heads end in `tanh`, so the model emits a bounded SHAPE and the model-external map multiplies
it by the calibrated scale. The loss is defined on the scaled LLR, which is the quantity the
quantizer sees at inference, so train and inference cannot disagree about the scale.

Auxiliary losses follow `design` section 3.2 point 3 and `memo_07` section 7. They are small and
they must exist: the scheduler's uplink SINR comes only from the post-equalization noise head, and
the classical measurement kernels need the channel estimate. `design` section 2.3 states the
criterion for them: the main head's coded BLER must not get worse with the auxiliary heads
attached, which is an ablation for the training stage (R2.4), not something this module can check.
"""

# The shared DSP utilities (dmrs, synth, labels, LDPC) still live in the AI-CE training tree; this is
# the single place that knows where, so the import below is not a hidden path dependency.
import depth3_paths
depth3_paths.add_ce_shared()
import argparse

import numpy as np

import depth3 as D3

# Target of the post-equalization noise head: the chain's own zero-forcing form
# `sigma2_post = sigma2 / |h|^2`, evaluated with the true channel. `equalize_zf_1xn.h` computes
# `sum(|h|^2 * sigma2) / (tx_scaling * sum(|h|^2))^2`, which at one port and unit scaling is that
# expression, and matching it keeps the reported SINR comparable with the classical path
# (`memo_07` N2) instead of merely plausible.
NOISE_SCALE_DECADES = 4.0
NOISE_SCALE_CENTER = -1.0

# Default weights. The main head dominates; the auxiliary terms exist to regularize the shared
# trunk, so they are small enough not to trade the main head away, and nonzero enough to be
# measured in the ablation.
DEFAULT_WEIGHTS = {"llr": 1.0, "ch_est": 0.1, "post_eq_noise": 0.01}


def llr_from_shape(shape, scale):
    """Map the head's tanh-bounded output to the LLR the chain quantizes."""
    return np.asarray(shape, dtype=np.float64) * float(scale)


def bce_loss(llr, labels, weights=None):
    """Mean binary cross entropy of the coded bits, over the weighted entries.

    `llr` is the log-likelihood ratio with the chain's sign (positive means bit 0), `labels` holds
    the BCE target `1 - b` on the same channels, and `weights` is the contract's validity mask. The
    stable form is `max(z, 0) - z * y + log1p(exp(-|z|))`, which is exact for large `|z|` and
    avoids the overflow that `log(sigmoid(z))` has at `z = -40`.
    """
    llr = np.asarray(llr, dtype=np.float64)
    labels = np.asarray(labels, dtype=np.float64)
    if llr.shape != labels.shape:
        raise ValueError(f"llr shape {llr.shape} does not match labels {labels.shape}")
    per_entry = np.maximum(llr, 0.0) - llr * labels + np.log1p(np.exp(-np.abs(llr)))
    if weights is None:
        return float(per_entry.mean())
    weights = np.asarray(weights, dtype=np.float64)
    total = weights.sum()
    if total <= 0.0:
        raise ValueError("the validity mask is empty: this capture has no coded bits to train on")
    return float((per_entry * weights).sum() / total)


def nmse(estimate, target, weights=None):
    """Normalized mean squared error of one complex field, as a linear ratio.

    Both sides are in units of the reference amplitude `r`, so the comparison is scale free by
    construction and needs no fitted gain (`build_synth_dataset.pair_nmse_db` needs one because it
    compares two independently scaled estimates).
    """
    estimate = np.asarray(estimate, dtype=np.complex128)
    target = np.asarray(target, dtype=np.complex128)
    error = np.abs(estimate - target) ** 2
    reference = np.abs(target) ** 2
    if weights is None:
        return float(error.sum() / (reference.sum() + 1e-30))
    weights = np.asarray(weights, dtype=np.float64)
    return float((error * weights).sum() / ((reference * weights).sum() + 1e-30))


def noise_shape_target(sigma2_post, scale_decades=NOISE_SCALE_DECADES,
                       center=NOISE_SCALE_CENTER):
    """Convert a linear post-equalization noise variance into the head's bounded target.

    The head carries `log10` of the variance between `center - scale_decades` and
    `center + scale_decades`, as the tanh-bounded shape `u = (log10(v) - center) / scale_decades`.
    Entries outside that window are NOT clipped into it: they are reported as invalid and excluded,
    because clipping would stack the deep fades on the boundary and teach the head to guess there.

    Returns `(u, valid)`.
    """
    sigma2_post = np.asarray(sigma2_post, dtype=np.float64)
    with np.errstate(divide="ignore", invalid="ignore"):
        u = (np.log10(sigma2_post) - center) / scale_decades
    valid = np.isfinite(u) & (np.abs(u) < 1.0)
    return np.clip(np.nan_to_num(u), -1.0, 1.0), valid


def noise_from_shape(u, scale_decades=NOISE_SCALE_DECADES, center=NOISE_SCALE_CENTER):
    """Inverse of `noise_shape_target`, i.e. the model-external map of the noise head."""
    return 10.0 ** (np.asarray(u, dtype=np.float64) * scale_decades + center)


def noise_regression_loss(u_pred, u_true, weights=None):
    """Mean squared error of the bounded noise shape on the entries the target covers."""
    u_pred = np.asarray(u_pred, dtype=np.float64)
    u_true = np.asarray(u_true, dtype=np.float64)
    per_entry = (u_pred - u_true) ** 2
    if weights is None:
        return float(per_entry.mean())
    weights = np.asarray(weights, dtype=np.float64)
    total = weights.sum()
    if total <= 0.0:
        raise ValueError("no valid post-equalization noise targets")
    return float((per_entry * weights).sum() / total)


def post_equalization_noise(channel, noise_variance):
    """The chain's zero-forcing post-equalization noise variance, for a known channel.

    `channel` is `[symbol, subcarrier]` complex, `noise_variance` is the scalar variance of the
    received samples. This is what the classical chain reports with a perfect estimate, so it is
    the target the noise head is trained against.
    """
    power = np.abs(np.asarray(channel, dtype=np.complex128)) ** 2
    with np.errstate(divide="ignore", invalid="ignore"):
        return np.where(power > 0.0, float(noise_variance) / power, np.inf)


def total_loss(llr, llr_labels, llr_weights, ch_est, ch_est_true, ch_est_weights,
               noise_shape, noise_true, noise_weights, weights=None):
    """The objective, with `design` section 3.2's priority order and defaults.

    A head that the graph does not carry is passed as None and simply has no term (design section 22.2): a bundle
    built without the reporting heads cannot be asked for a channel-estimate error it never predicted, and a zero
    there would be a number nobody measured.
    """
    weights = dict(DEFAULT_WEIGHTS if weights is None else weights)
    terms = {"llr": bce_loss(llr, llr_labels, llr_weights)}
    if ch_est is not None:
        terms["ch_est"] = nmse(ch_est, ch_est_true, ch_est_weights)
    if noise_shape is not None:
        terms["post_eq_noise"] = noise_regression_loss(noise_shape, noise_true, noise_weights)
    total = sum(weights[name] * terms[name] for name in terms if name in weights)
    terms["total"] = float(total)
    return terms


def tf_bce_loss(llr, labels, weights=None):
    """TensorFlow form of `bce_loss`, for the training loop.

    Kept thin on purpose: `--selfcheck` evaluates both implementations on the same random inputs,
    so a drift between the numpy reference and the one the trainer runs is a test failure rather
    than a silent change of objective.
    """
    import tensorflow as tf

    per_entry = tf.nn.sigmoid_cross_entropy_with_logits(labels=labels, logits=llr)
    if weights is None:
        return tf.reduce_mean(per_entry)
    weights = tf.cast(weights, per_entry.dtype)
    return tf.reduce_sum(per_entry * weights) / tf.maximum(tf.reduce_sum(weights), 1e-12)


def tf_nmse(estimate, target, weights=None):
    """TensorFlow form of `nmse`."""
    import tensorflow as tf

    error = tf.math.real(estimate - target) ** 2 + tf.math.imag(estimate - target) ** 2
    reference = tf.math.real(target) ** 2 + tf.math.imag(target) ** 2
    if weights is None:
        return tf.reduce_sum(error) / (tf.reduce_sum(reference) + 1e-30)
    weights = tf.cast(weights, error.dtype)
    return tf.reduce_sum(error * weights) / (tf.reduce_sum(reference * weights) + 1e-30)


def selfcheck():
    """Check the objective's identities, including the one whose failure looks like slow training."""
    rng = np.random.default_rng(20261013)
    scale = 20.0

    # A coded-bit sequence, half zeros and half ones so no class weighting is needed.
    bits = rng.integers(0, 2, size=4096).astype(np.float64)
    labels = 1.0 - bits
    weights = np.ones_like(bits)

    # 1. Perfect prediction: the LLR saturates in the correct direction and the loss vanishes.
    perfect = (2.0 * labels - 1.0) * 30.0
    loss_perfect = bce_loss(perfect, labels, weights)
    assert loss_perfect < 1e-9, f"a perfect model scores {loss_perfect}, not zero"

    # 2. No information: zero logits score exactly ln 2.
    loss_zero = bce_loss(np.zeros_like(bits), labels, weights)
    assert abs(loss_zero - np.log(2.0)) < 1e-12, f"zero logits score {loss_zero}, not ln 2"

    # 3. Saturated and wrong: the loss is the scale, which is the ceiling the objective can report.
    wrong = -(2.0 * labels - 1.0) * scale
    loss_wrong = bce_loss(wrong, labels, weights)
    assert abs(loss_wrong - scale) < 1e-6, f"a saturated wrong model scores {loss_wrong}"

    # 4. The gradient at a zero logit is sigma(0) - y, as `design` section 3.2 states. Its sign is
    #    what pushes a bit-0 logit positive, so this is the sign check in its analytic form.
    delta = 1e-6
    for label in (0.0, 1.0):
        up = bce_loss(np.array([delta]), np.array([label]))
        down = bce_loss(np.array([-delta]), np.array([label]))
        numeric = (up - down) / (2.0 * delta)
        assert abs(numeric - (0.5 - label)) < 1e-6, (numeric, label)
    assert bce_loss(np.array([0.0]), np.array([1.0])) > 0.0

    # 5. The inverted-label failure is detectable, which is the point of the label convention.
    loss_inverted = bce_loss(perfect, bits, weights)
    assert loss_inverted > scale - 1e-6, (
        "with the label inverted a perfect model must score badly; the check cannot see the "
        "inversion it exists for")

    # 6. The noise head's map is invertible over its window, and outside the window the targets are
    #    excluded rather than piled onto the boundary.
    variances = np.array([1e-6, 1e-4, 1e-2, 1.0, 1e2, 1e4])
    u, valid = noise_shape_target(variances)
    recovered = noise_from_shape(u[valid])
    assert np.allclose(recovered, variances[valid], rtol=1e-6), (recovered, variances[valid])
    assert not valid[0] and not valid[-1], "the window is not excluding the extremes"
    assert noise_regression_loss(u, u, valid.astype(float)) == 0.0

    # 7. The channel-estimate NMSE is zero on a perfect estimate and scale free.
    channel = (rng.standard_normal((14, 612)) + 1j * rng.standard_normal((14, 612)))
    assert nmse(channel, channel) == 0.0
    assert abs(nmse(2.0 * channel, channel)) > 0.1, "the NMSE is not scale free"

    # 8. The post-equalization noise target is the chain's form, and it is the reciprocal of the
    #    channel power at unit noise.
    power = np.abs(channel) ** 2
    assert np.allclose(post_equalization_noise(channel, 1.0), 1.0 / power), "ZF noise form"

    # 9. The TensorFlow implementations agree with the numpy reference on the same inputs.
    import tensorflow as tf

    llr_tf = tf.constant(rng.standard_normal(4096) * 5.0)
    labels_tf = tf.constant(labels)
    weights_tf = tf.constant(weights)
    tf_loss = float(tf_bce_loss(llr_tf, labels_tf, weights_tf))
    np_loss = bce_loss(llr_tf.numpy(), labels, weights)
    assert abs(tf_loss - np_loss) < 1e-5, (tf_loss, np_loss)

    est_tf = tf.constant(channel / 3.0)
    tgt_tf = tf.constant(channel)
    assert abs(float(tf_nmse(est_tf, tgt_tf)) - nmse(channel / 3.0, channel)) < 1e-9

    # 10. The total objective weights the three terms as declared, so the ablation can turn the
    #     auxiliary terms off by setting their weights to zero.
    terms = total_loss(perfect, labels, weights, channel, channel, None,
                       u, u, valid.astype(float))
    assert terms["llr"] < 1e-9 and terms["ch_est"] == 0.0 and terms["post_eq_noise"] == 0.0
    assert terms["total"] < 1e-9, terms["total"]

    # 11. The head's bounded shape reaches the same LLR as the scale implies, which is what makes
    #     training on `tanh` outputs consistent with quantizing `shape * scale` at inference.
    shape = np.tanh(perfect / scale)
    assert np.allclose(llr_from_shape(shape, scale), np.tanh(perfect / scale) * scale)
    quantized = D3.llr_from_logits(shape, "64QAM", scale=scale)
    assert quantized.min() >= -D3.LLR_MAX and quantized.max() <= D3.LLR_MAX

    print(f"depth3_loss selfcheck: perfect={loss_perfect:.3e} zero={loss_zero:.6f} "
          f"(ln2={np.log(2.0):.6f}) saturated-wrong={loss_wrong:.3f} scale={scale}")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selfcheck", action="store_true",
                        help="check the objective's identities and the numpy/TF agreement")
    args = parser.parse_args()
    if args.selfcheck:
        return selfcheck()
    parser.error("nothing to do: pass --selfcheck")


if __name__ == "__main__":
    raise SystemExit(main())
