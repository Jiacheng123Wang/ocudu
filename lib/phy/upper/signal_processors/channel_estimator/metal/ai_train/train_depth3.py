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

import tensorflow as tf

import dd_label as DL              # noqa: E402
import depth3 as D3                # noqa: E402
import depth3_loss as L            # noqa: E402
import ldpc_decode as LD           # noqa: E402
import synth as SN                 # noqa: E402
# The canonical LayerScale for this workstream lives next to the refiner that needed it first.
from train_refiner import Scale    # noqa: E402

NOF_SYMBOLS = D3.NOF_SYMBOLS
DILATIONS = (1, 2, 4, 8)


@tf.keras.utils.register_keras_serializable(package="ocudu")
class FoldedScaleConv(tf.keras.layers.Layer):
    """A 1x1 convolution whose output channels carry a trainable positive gain.

    `gamma * relu(W x + b)` equals `relu((gamma W) x + gamma b)` while `gamma` stays positive, so
    the LayerScale the residual blocks need costs no operator of its own: the gain folds into the
    convolution's weights. A separate multiply doubles the operator count on the ANE, which
    section 45.8 measured as about 200 us across 18 blocks.

    The gain is parameterised through softplus, which keeps it strictly positive without a
    constraint, and the bias is scaled with it because the scale sits after the affine map.
    """

    def __init__(self, filters, initial_gain=0.1, **kwargs):
        super().__init__(**kwargs)
        self.filters = int(filters)
        self.initial_gain = float(initial_gain)

    def get_config(self):
        return {**super().get_config(), "filters": self.filters,
                "initial_gain": self.initial_gain}

    def build(self, input_shape):
        channels = int(input_shape[-1])
        self.kernel = self.add_weight(name="kernel", shape=(1, 1, channels, self.filters),
                                      initializer="glorot_uniform", trainable=True)
        self.bias = self.add_weight(name="bias", shape=(self.filters,), initializer="zeros",
                                    trainable=True)
        # softplus(x) = initial_gain, so training starts where a plain LayerScale would.
        inverse = float(np.log(np.expm1(self.initial_gain)))
        self.gain = self.add_weight(name="gain", shape=(self.filters,),
                                    initializer=tf.keras.initializers.Constant(inverse),
                                    trainable=True)

    def call(self, inputs):
        gamma = tf.nn.softplus(self.gain)
        kernel = self.kernel * gamma[None, None, None, :]
        return tf.nn.conv2d(inputs, kernel, strides=1, padding="SAME") + gamma * self.bias


def build(width=D3.NOF_SUBCARRIERS, blocks=18, qm=8, aux_heads=True, c_hid=64, residual=True,
          dtype="float32", fold_scale=True):
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
    keras_dtype = tf.float32 if dtype == "float32" else tf.float16
    inp = tf.keras.Input(shape=(width, 1, D3.NOF_CHANNELS), batch_size=None, dtype=keras_dtype,
                         name="x")
    y = inp
    for i in range(blocks):
        dilation = DILATIONS[i % len(DILATIONS)]
        block = tf.keras.layers.DepthwiseConv2D(3, padding="same", dilation_rate=(dilation, 1),
                                                dtype=keras_dtype, name=f"dw{i}_3x3_d{dilation}")(y)
        block = tf.keras.layers.ReLU(dtype=keras_dtype, name=f"relu{i}")(block)
        if residual and fold_scale:
            block = FoldedScaleConv(c_hid, dtype=keras_dtype, name=f"pw{i}_1x1")(block)
            block = tf.keras.layers.ReLU(dtype=keras_dtype, name=f"relu{i}b")(block)
            y = tf.keras.layers.Add(name=f"add{i}", dtype=keras_dtype)([y, block])
        else:
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
        # The fourth head is one scalar for the whole slot, so the trunk is averaged over frequency
        # first. The contract lists it as its own output because `memo_07` R4 needs the DM-RS domain
        # noise variance, which the classical kernel can no longer measure once the estimate is the
        # network's own: the residual it would measure has no noise left in it.
        pooled = tf.keras.layers.GlobalAveragePooling2D(keepdims=True, dtype=keras_dtype,
                                                        name="dmrs_pool")(y)
        outputs["dmrs_noise"] = tf.keras.layers.Activation(
            "tanh", dtype=keras_dtype, name="dmrs_noise")(
                tf.keras.layers.Conv2D(1, 1, dtype=keras_dtype, name="dmrs_noise_1x1")(pooled))
    return tf.keras.Model(inp, outputs)


class Batches:
    """Builds training tensors from a dataset file, on the fly.

    The label planes are derived per sample from the stored coded bits and masks instead of being
    stored, so the file stays small and the mapping from bits to channels has exactly one
    implementation (`depth3.label_plane`).
    """

    ARRAYS = ("X", "H", "bits", "nof_bits", "alloc_first_prb", "alloc_nof_prb", "dmrs_bitmap",
              "qm", "sigma2_over_r2", "snr_nominal", "tb_bits", "nof_tb_bits", "rnti", "n_id",
              "rv")

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

    def allocation_of(self, index):
        first_prb = int(self.data["alloc_first_prb"][index])
        return list(range(first_prb, first_prb + int(self.data["alloc_nof_prb"][index])))

    def dmrs_symbols_of(self, index):
        bitmap = int(self.data["dmrs_bitmap"][index])
        return [s for s in range(NOF_SYMBOLS) if (bitmap >> s) & 1]

    def modulation_of(self, index):
        return {v: k for k, v in D3.BITS_PER_SYMBOL.items()}[int(self.data["qm"][index])]

    def sample(self, index):
        data = self.data
        alloc = self.allocation_of(index)
        dmrs_symbols = self.dmrs_symbols_of(index)
        modulation = self.modulation_of(index)
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
            "index": index,
            "x": plane[:, None, :],
            "llr_shape": label,
            "llr_weights": llr_weights,
            "ch": np.stack([channel.real, channel.imag], axis=-1),
            "ch_weights": ch_weights,
            "noise": noise_target[0],
            "noise_weights": noise_weights.astype(np.float32),
            "sigma2_over_r2": np.float32(sigma2_norm),
            # The DM-RS domain noise variance is the same scalar the post-equalization head is
            # derived from, so its target is the same `log10` shape of it, computed on the scalar
            # rather than per resource element.
            "dmrs_noise": np.float32(L.noise_shape_target(np.asarray([sigma2_norm]))[0][0]),
            "snr": float(data["snr_nominal"][index]),
        }

    def epoch(self, batch_size):
        """Yield batches for one epoch, reshuffling first."""
        self.rng.shuffle(self.order)
        for start in range(0, self.n, batch_size):
            indices = self.order[start:start + batch_size]
            yield [self.sample(int(i)) for i in indices]


class CodedBler:
    """Coded block error rate of the main head, through the real LDPC code.

    Uncoded BER says the LLRs point the right way; only decoding says whether the code can still
    correct what they got wrong, and `design` section 2.3 makes coded BLER the criterion. Each
    sample is decoded block by block: the LLRs are descrambled, deinterleaved and deposited into
    the codeblock's circular buffer, then decoded, and a block counts as an error unless
    re-encoding the decoded word reproduces the transmitted bits exactly. That last step is what a
    receiver's codeblock CRC decides, and it needs no reference codeword.

    The decoders are cached per (base graph, lifting size): building the QC structures costs more
    than decoding one block.
    """

    def __init__(self, batches, max_iterations=25):
        self.batches = batches
        self.decoders = {}
        self.max_iterations = max_iterations
        self.blocks = 0
        self.errors = 0
        self.skipped = 0

    def decoder(self, bg, zc):
        key = (int(bg), int(zc))
        if key not in self.decoders:
            self.decoders[key] = LD.MinSum(bg, zc, max_iterations=self.max_iterations)
        return self.decoders[key]

    def add(self, index, llr_chain):
        """Decode one sample's LLRs, in the chain's codeword order."""
        data = self.batches.data
        alloc = self.batches.allocation_of(index)
        dmrs_symbols = self.batches.dmrs_symbols_of(index)
        modulation = self.batches.modulation_of(index)
        qm = D3.BITS_PER_SYMBOL[modulation]
        nof_tb = int(data["nof_tb_bits"][index])
        tb_bits = np.unpackbits(data["tb_bits"][index])[:nof_tb]
        transmitted = np.unpackbits(data["bits"][index])[:int(data["nof_bits"][index])]
        if transmitted.size != llr_chain.size:
            raise ValueError(f"{llr_chain.size} LLRs for {transmitted.size} transmitted bits")

        dmrs_mask = sum(1 << s for s in dmrs_symbols)
        code_blocks, bg, zc, kb, e_total, lengths = SN.codeblock_layout(
            tb_bits, len(alloc), NOF_SYMBOLS, qm, dmrs_mask)
        rv = int(data["rv"][index])
        sequence = np.asarray(DL.gold_seq((int(data["rnti"][index]) << 15)
                                          + int(data["n_id"][index]), transmitted.size),
                              dtype=np.uint8)
        descrambled = llr_chain * (1.0 - 2.0 * sequence.astype(np.float64))
        # The scrambling sequence covers the whole codeword, so the comparison below stays in the
        # descrambled domain. Re-scrambling one block on its own would restart the sequence at
        # position 0 and mismatch everything except the first block, which is exactly how this
        # looked before it was fixed: every block decoded exactly and five of six "failed".
        descrambled_bits = transmitted ^ sequence
        decoder = self.decoder(bg, zc)

        offset = 0
        for block, e_cb in enumerate(lengths):
            block_llr = descrambled[offset:offset + e_cb]
            offset += e_cb
            # A retransmission that carries almost no systematic bits cannot be decoded on its own:
            # the receiver is expected to combine it with an earlier transmission first. Counting
            # it as a block error would report the HARQ design as a network failure.
            if LD.systematic_share(len(code_blocks[block]), kb, zc, e_cb, rv, bg) < 0.2:
                self.skipped += 1
                continue
            buffer, _ = LD.rate_dematch(LD.deinterleave(block_llr, qm), len(code_blocks[block]), kb,
                                        zc, e_cb, rv, bg)
            decoded, converged, _ = decoder.decode(buffer)
            # Re-encoding the decoded word and comparing with what was transmitted is the same
            # decision a codeblock CRC makes, and it needs no reference codeword.
            rematched = np.asarray(DL.rate_match(list(decoded), len(code_blocks[block]), kb, zc,
                                                 e_cb, rv, bg), dtype=np.uint8)
            rematched = np.asarray(DL.bit_interleave(rematched.tolist(), qm), dtype=np.uint8)
            self.blocks += 1
            self.errors += int(not np.array_equal(rematched,
                                                  descrambled_bits[offset - e_cb:offset]))
        assert offset == transmitted.size, (offset, transmitted.size)

    def rate(self):
        """Block error rate over the blocks that are decodable alone, and how many were not."""
        if not self.blocks:
            return float("nan")
        return self.errors / self.blocks

    def summary(self):
        return f"{self.rate():.4f} over {self.blocks} blocks ({self.skipped} skipped as HARQ)"


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


def rename_outputs(mlmodel, qm):
    """Rename a converted model's outputs to the contract's names.

    The TensorFlow frontend names the outputs `Identity`, `Identity_1`, ... and loses the Keras
    layer names, while the contract binds them BY NAME. Renaming the spec is what makes the
    artifact self-describing, and it is checked downstream: `depth3_engine_check` refuses a bundle
    whose heads are not named `llr`, `post_eq_noise`, `ch_est` and `dmrs_noise`.

    The heads are matched by their CHANNEL COUNT and not by their order, because the order the
    converter emits them in is an implementation detail of the frontend. The counts are unique
    (`14 * Qm`, 14, 28 and 1), so a mismatch is detectable rather than silently bound to the wrong
    head: binding the channel estimate to the LLR head would produce a receiver that decodes
    garbage from an artifact that looks correct.
    """
    import coremltools as ct
    spec     = mlmodel.get_spec()
    expected = {NOF_SYMBOLS * qm: "llr", NOF_SYMBOLS: "post_eq_noise",
                2 * NOF_SYMBOLS: "ch_est", 1: "dmrs_noise"}
    actual   = list(spec.description.output)
    if len(actual) != len(expected):
        raise ValueError(f"the model has {len(actual)} outputs, the contract has {len(expected)}")
    for output in actual:
        channels = output.type.multiArrayType.shape[-1]
        name     = expected.pop(channels, None)
        if name is None:
            raise ValueError(f"no contract head has {channels} channels")
        ct.utils.rename_feature(spec, output.name, name)
    return ct.models.MLModel(spec, weights_dir=mlmodel.weights_dir)


def export_deployment(model, qm, path):
    """Convert a TRAINED model to the fp16 deployment graph, with the contract's output names.

    The conversion decides the deployment precision, which is why training runs in fp32: the
    artifact is fp16 either way. The heads are renamed because the TensorFlow frontend emits
    `Identity`, `Identity_1`, ... and drops the Keras layer names, while the contract binds them BY
    NAME; `depth3_engine_check` refuses a bundle whose heads are not named `llr`, `post_eq_noise`,
    `ch_est` and `dmrs_noise`, and a bundle exported before training would carry random weights and
    still pass every shape check.
    """
    import coremltools as ct
    converted = ct.convert(
        model,
        inputs=[ct.TensorType(name="x", shape=(1, D3.NOF_SUBCARRIERS, 1, D3.NOF_CHANNELS),
                              dtype=np.float16)],
        compute_precision=ct.precision.FLOAT16,
        minimum_deployment_target=ct.target.iOS17,
        convert_to="mlprogram")
    converted = rename_outputs(converted, qm)
    converted.save(path)
    print(f"wrote the fp16 deployment graph to {path}")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--train", default="", help="training dataset; not needed with --export-only")
    parser.add_argument("--val", default="", help="validation dataset; not needed with --export-only")
    parser.add_argument("--out", default="", help="run directory; not needed with --export-only")
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
    parser.add_argument("--no-fold-scale", action="store_true",
                        help="keep the LayerScale as a separate multiply. Costs about 200 us on "
                             "the ANE (section 45.8); the default folds it into the convolution")
    parser.add_argument("--export-only", default="",
                        help="export this trained .keras to the fp16 deployment graph and exit")
    parser.add_argument("--export-probe", default="",
                        help="also write the graph as a CoreML mlpackage, so the ANE probe "
                             "measures the artifact this script trains rather than a copy of it")
    parser.add_argument("--no-residual", action="store_true",
                        help="plain block stack. It collapses (section 12.9); kept so the "
                             "failure can be reproduced rather than remembered")
    parser.add_argument("--dtype", choices=("float32", "float16"), default="float32",
                        help="training dtype. The exported artifact is fp16 regardless")
    parser.add_argument("--bler-every", type=int, default=5,
                        help="evaluate coded BLER every N epochs. Decoding dominates the "
                             "validation cost, so it is sampled rather than run every epoch")
    parser.add_argument("--bler-check", action="store_true",
                        help="decode PERFECT LLRs on a few validation samples and exit. The rate "
                             "must be zero, which is what proves the metric can reach zero rather "
                             "than merely being small")
    parser.add_argument("--bler-samples", type=int, default=24,
                        help="validation samples to decode for the coded BLER. Decoding is far "
                             "more expensive than a forward pass, so this is a subset; 0 skips it")
    parser.add_argument("--seed", type=int, default=20261013)
    args = parser.parse_args()

    tf.get_logger().setLevel("ERROR")
    tf.random.set_seed(args.seed)

    if args.export_only:
        exported = tf.keras.models.load_model(
            args.export_only, compile=False,
            custom_objects={"FoldedScaleConv": FoldedScaleConv})
        if not args.export_probe:
            parser.error("--export-only needs --export-probe")
        export_deployment(exported, D3.BITS_PER_SYMBOL[args.modulation], args.export_probe)
        return 0

    if not args.train or not args.val or not args.out:
        parser.error("--train, --val and --out are required unless --export-only is given")
    train = Batches(args.train, args.modulation, args.max_train, seed=args.seed)
    val = Batches(args.val, args.modulation, args.max_val, seed=args.seed + 1)
    qm = train.qm
    print(f"train {train.n} samples, val {val.n} samples, {args.modulation} (Qm={qm})")

    model = build(blocks=args.blocks, qm=qm, aux_heads=not args.no_aux,
                  residual=not args.no_residual, dtype=args.dtype,
                  fold_scale=not args.no_fold_scale)
    parameters = int(sum(np.prod(w.shape) for w in model.weights))
    print(f"parameters {parameters}")

    # The scale is model-external per the contract, so the loss sees the LLR the quantizer sees.
    scale = D3.RANGE_LIMIT_FLOAT[args.modulation]
    optimizer = tf.keras.optimizers.Adam(args.lr)
    weights = {"llr": args.w_llr, "ch_est": args.w_ch, "post_eq_noise": args.w_noise,
               "dmrs_noise": args.w_noise}

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
                dmrs = tf.cast(outputs["dmrs_noise"], tf.float32)[:, 0, 0, 0]
                terms["dmrs_noise"] = tf.reduce_mean((dmrs - targets["dmrs_noise"]) ** 2)
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
                "dmrs_noise": tf.constant(stack(samples, "dmrs_noise")),
            },
        )

    def perfect_llr(sample):
        """The LLRs a perfect model would emit, from the contract's label plane.

        The label is `1 - b` and a positive LLR means bit 0, so `(2 * label - 1) * large` is the
        ideal LLR on every channel that carries a coded bit.
        """
        return (2.0 * sample["llr_shape"] - 1.0) * LD.KNOWN_LLR

    def coded_rate(samples, use_perfect):
        """The coded BLER of one batch of samples, capped by `--bler-samples`."""
        bler = CodedBler(val)
        for row, sample in enumerate(samples):
            index = int(sample["index"])
            plane = perfect_llr(sample) if use_perfect else sample["llr_prediction"]
            bler.add(index, D3.llr_gather(plane, val.allocation_of(index),
                                          val.dmrs_symbols_of(index), val.modulation_of(index)))
        return bler.rate()

    if args.bler_check:
        bler = CodedBler(val)
        checked = 0
        for samples in val.epoch(args.batch):
            for index_sample in range(len(samples)):
                if checked >= max(1, min(args.bler_samples, 8)):
                    break
                index = int(samples[index_sample]["index"])
                bler.add(index, D3.llr_gather(perfect_llr(samples[index_sample]),
                                              val.allocation_of(index), val.dmrs_symbols_of(index),
                                              val.modulation_of(index)))
                checked += 1
            break
        print(f"coded BLER on perfect LLRs: {bler.summary()}")
        assert bler.blocks > 0, "no decodable block in the validation set"
        assert bler.rate() == 0.0, (
            "the metric cannot reach zero, so it would never accept a good model")
        return 0

    def evaluate(use_bler):
        totals, errors, magnitudes = [], [], []
        bler = CodedBler(val) if use_bler else None
        decoded = 0
        for samples in val.epoch(args.batch):
            x, targets = tensors(samples)
            total, _, llr = step(x, targets, training=False)
            ber, mag = decode_metrics(llr.numpy(), targets["llr_shape"].numpy(),
                                      targets["llr_weights"].numpy())
            totals.append(float(total))
            errors.append(ber)
            magnitudes.append(mag)
            if bler is not None and decoded < args.bler_samples:
                for row in range(llr.shape[0]):
                    if decoded >= args.bler_samples:
                        break
                    index = int(samples[row]["index"])
                    gathered = D3.llr_gather(llr.numpy()[row], val.allocation_of(index),
                                             val.dmrs_symbols_of(index), val.modulation_of(index))
                    bler.add(index, gathered)
                    decoded += 1
        return (float(np.mean(totals)), float(np.mean(errors)), float(np.mean(magnitudes)),
                bler.rate() if bler is not None else float("nan"),
                bler.skipped if bler is not None else 0)

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
        want_bler = args.bler_samples > 0 and (epoch % max(1, args.bler_every) == 0
                                               or epoch == args.epochs)
        val_loss, val_ber, val_mag, val_bler, val_skipped = evaluate(want_bler)
        history.append({"epoch": epoch, "train": float(np.mean(losses)), "val": val_loss,
                        "val_ber": val_ber, "val_llr_mean": val_mag, "val_bler": val_bler,
                        "val_bler_skipped": val_skipped})
        if epoch == 1 or epoch % max(1, args.epochs // 20) == 0 or epoch == args.epochs:
            print(f"epoch {epoch:4d}  train {np.mean(losses):.5f}  val {val_loss:.5f}  "
                  f"uncoded BER {val_ber:.4f}  coded BLER {val_bler:.4f}  "
                  f"mean|LLR| {val_mag:6.1f}  ({time.time() - started:.0f}s)")
        if val_loss < best:
            best = val_loss
            model.save(os.path.join(args.out, "depth3_best.keras"))

    with open(os.path.join(args.out, "history.json"), "w") as handle:
        json.dump({"history": history, "parameters": parameters,
                   "weights": weights, "scale": scale, "modulation": args.modulation}, handle,
                  indent=1)
    np.save(os.path.join(args.out, "val_llr_stats.npy"),
            np.array([[h["val_ber"], h["val_llr_mean"], h["val_bler"]] for h in history]))
    print(f"best val loss {best:.5f}; wrote {args.out}/depth3_best.keras and history.json")

    # Export AFTER training, from the trained weights. Exporting first would produce a deployment
    # graph with random weights that passes every shape check, which is the kind of artifact that
    # reaches a receiver and produces plausible nonsense.
    if args.export_probe:
        model.load_weights(os.path.join(args.out, "depth3_best.keras"))
        export_deployment(model, qm, args.export_probe)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
