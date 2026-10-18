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
          dtype="float32", fold_scale=True, scale_init=1.0, llr_linear=False, dense=False,
          layout="folded", in_channels=D3.NOF_CHANNELS_SPATIAL, cond=False):
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

    def finish(graph):
        """Marks the CONTRACT the graph follows, for the export's metadata declaration (design section 19.102).

        A conditioned bundle and a per-modulation 256QAM bundle have the same main-head width, so the bundle has
        to SAY which one it is; an attribute here is what lets the export write that down.
        """
        graph._depth3_cond = bool(cond)
        return graph

    # The MODULATION-CONDITIONED model (design section 19.102): one bundle for all four modulations, with the
    # Qm as an INPUT (four one-hot channels appended to the plane) and a main head whose stride is the widest
    # modulation, so a reception reads `symbol * COND_STRIDE + bit` for its own bits and ignores the rest.
    # The conditioning channels are APPENDED BY THE DATASET (see Batches.sample), because only the sample knows
    # its own Qm -- so `in_channels` here is already the data's channel count and must not be widened again. The
    # build()/data check in main() is what caught the first version of this: it refused 64 against 68.
    head_qm = D3.COND_STRIDE if cond else qm
    if layout != "folded":
        # Time is a spatial axis here, so the 3x3 kernel mixes frequency AND symbols. `in_channels`
        # is the packing the data carries: 12 for the contract's spatial plane, 8 for the R4.4
        # diagnostic arm that isolated the locality.
        inp = tf.keras.Input(shape=(width, NOF_SYMBOLS, in_channels), batch_size=None,
                             dtype=keras_dtype, name="x")
        y = tf.keras.layers.Conv2D(c_hid, 1, dtype=keras_dtype, name="entry_1x1")(inp)
        y = tf.keras.layers.ReLU(dtype=keras_dtype, name="entry_relu")(y)
        for i in range(blocks):
            dilation = DILATIONS[i % len(DILATIONS)]
            block = tf.keras.layers.DepthwiseConv2D(
                3, padding="same", dilation_rate=(dilation, 1), dtype=keras_dtype,
                name=f"dw{i}_3x3_d{dilation}")(y)
            block = tf.keras.layers.ReLU(dtype=keras_dtype, name=f"relu{i}")(block)
            block = FoldedScaleConv(c_hid, initial_gain=scale_init, dtype=keras_dtype,
                                    name=f"pw{i}_1x1")(block)
            block = tf.keras.layers.ReLU(dtype=keras_dtype, name=f"relu{i}b")(block)
            y = tf.keras.layers.Add(name=f"add{i}", dtype=keras_dtype)([y, block])
        # The heads keep the CONTRACT's channel counts (14*Qm, 14, 28, 1) whatever the trunk's
        # internal layout is. The seam binds them by name and the engine checks them by channel
        # count, so a layout that renamed them would be a contract change rather than a choice
        # inside the network. Only the spatial axes differ, and every one of them is flattened back
        # to the contract's `[width, channels]` before the head leaves the model.
        llr = tf.keras.layers.Conv2D(head_qm, 1, dtype=keras_dtype, name="llr_1x1")(y)
        llr = tf.keras.layers.Activation("linear" if llr_linear else "tanh", dtype=keras_dtype,
                                         name="llr")(llr)
        # [batch, width, symbols, qm] -> [batch, width, symbols * qm]: the contract's channel order,
        # symbol major and bit fastest, which is what the label plane already holds.
        outputs = {"llr": tf.keras.layers.Reshape((width, NOF_SYMBOLS * head_qm), name="llr_flat")(llr)}
        if aux_heads:
            noise = tf.keras.layers.Activation("tanh", dtype=keras_dtype, name="post_eq_noise")(
                tf.keras.layers.Conv2D(1, 1, dtype=keras_dtype, name="noise_1x1")(y))
            outputs["post_eq_noise"] = tf.keras.layers.Reshape((width, NOF_SYMBOLS),
                                                               name="noise_flat")(noise)
            # Linear for the channel: it must reach deep fades as well as peaks.
            ch_est = tf.keras.layers.Activation("linear", dtype=keras_dtype, name="ch_est")(
                tf.keras.layers.Conv2D(2, 1, dtype=keras_dtype, name="ch_est_1x1")(y))
            outputs["ch_est"] = tf.keras.layers.Reshape((width, 2 * NOF_SYMBOLS),
                                                        name="ch_est_flat")(ch_est)
            pooled = tf.keras.layers.GlobalAveragePooling2D(keepdims=True, dtype=keras_dtype,
                                                            name="dmrs_pool")(y)
            outputs["dmrs_noise"] = tf.keras.layers.Activation(
                "tanh", dtype=keras_dtype, name="dmrs_noise")(
                    tf.keras.layers.Conv2D(1, 1, dtype=keras_dtype, name="dmrs_noise_1x1")(pooled))
        return finish(tf.keras.Model(inp, outputs))

    inp = tf.keras.Input(shape=(width, 1, in_channels), batch_size=None, dtype=keras_dtype,
                         name="x")
    y = inp
    if int(inp.shape[-1]) != c_hid:
        # The residual adds need matching widths, and the contract's plane has 64 channels whatever
        # the trunk uses. One projection at the entry is cheaper than a skip projection per block.
        y = tf.keras.layers.Conv2D(c_hid, 1, dtype=keras_dtype, name="entry_1x1")(y)
        y = tf.keras.layers.ReLU(dtype=keras_dtype, name="entry_relu")(y)
    for i in range(blocks):
        dilation = DILATIONS[i % len(DILATIONS)]
        # A DEPTHWISE 3x3 mixes frequency within each channel; a dense one mixes channels and
        # frequency together, which is far more expressive per layer. `model_network_design` 25.3
        # measured that a dense 3x3 computes 5.7 times the FLOPs for the SAME latency on this
        # engine, so the depthwise choice was buying an arithmetic-intensity saving the ANE did not
        # need, at the cost of exactly the per-position capacity the diagnosis points at.
        if dense:
            block = tf.keras.layers.Conv2D(c_hid, 3, padding="same", dilation_rate=(dilation, 1),
                                           dtype=keras_dtype, name=f"cv{i}_3x3_d{dilation}")(y)
        else:
            block = tf.keras.layers.DepthwiseConv2D(
                3, padding="same", dilation_rate=(dilation, 1), dtype=keras_dtype,
                name=f"dw{i}_3x3_d{dilation}")(y)
        block = tf.keras.layers.ReLU(dtype=keras_dtype, name=f"relu{i}")(block)
        if residual and fold_scale:
            block = FoldedScaleConv(c_hid, initial_gain=scale_init, dtype=keras_dtype,
                                    name=f"pw{i}_1x1")(block)
            block = tf.keras.layers.ReLU(dtype=keras_dtype, name=f"relu{i}b")(block)
            y = tf.keras.layers.Add(name=f"add{i}", dtype=keras_dtype)([y, block])
        else:
            block = tf.keras.layers.Conv2D(c_hid, 1, dtype=keras_dtype, name=f"pw{i}_1x1")(block)
            block = tf.keras.layers.ReLU(dtype=keras_dtype, name=f"relu{i}b")(block)
            if residual:
                block = Scale(initial=scale_init, name=f"scale{i}", dtype=keras_dtype)(block)
                y = tf.keras.layers.Add(name=f"add{i}", dtype=keras_dtype)([y, block])
            else:
                y = block

    # The main head's bound. `tanh` keeps the output inside +-1 with no clipping branch, but its
    # gradient vanishes exactly where a confident bit lives: at 42 dB the correct LLR is far beyond
    # the scale, so every well-classified bit sits in the flat region and stops teaching the trunk.
    # Section 4.2 lists a clamp as the second allowed form of the same bound, and a linear head with
    # an explicit clamp keeps the gradient everywhere the output is not already clipped.
    llr_head = tf.keras.layers.Conv2D(NOF_SYMBOLS * head_qm, 1, dtype=keras_dtype, name="llr_1x1")(y)
    outputs = {"llr": tf.keras.layers.Activation(
        "linear" if llr_linear else "tanh", dtype=keras_dtype, name="llr")(llr_head)}
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
    return finish(tf.keras.Model(inp, outputs))


class Batches:
    """Builds training tensors from a dataset file, on the fly.

    The label planes are derived per sample from the stored coded bits and masks instead of being
    stored, so the file stays small and the mapping from bits to channels has exactly one
    implementation (`depth3.label_plane`).
    """

    ARRAYS = ("X", "H", "bits", "nof_bits", "alloc_first_prb", "alloc_nof_prb", "dmrs_bitmap",
              "qm", "sigma2_over_r2", "snr_nominal", "tb_bits", "nof_tb_bits", "rnti", "n_id",
              "rv")

    def __init__(self, path, modulation, max_samples=0, seed=0, snr_min=None, snr_max=None, cond=False,
                 target="coded", layout="folded"):
        # Read every array once, eagerly. The file is a compressed archive, and accessing
        # `data["X"]` per sample would decompress the whole 200 MB array every time.
        archive = np.load(path, allow_pickle=True)
        self.data = {name: archive[name] for name in self.ARRAYS}
        self.modulation = modulation
        self.qm = D3.BITS_PER_SYMBOL[modulation]
        # A CONDITIONED dataset feeds one model that serves every modulation: the samples carry their own Qm (the
        # file already does), the label plane uses the stride-8 layout, and the Qm is appended as a one-hot.
        self.cond = cond
        # "coded" trains against the transmitted bits, which is the contract's task. "genie" trains
        # against the hard decisions a receiver with the TRUE channel would make, which needs only a
        # per-resource-element division and a constellation decision and no interpolation across
        # frequency. The difference between the two isolates where the network stalls.
        self.target = target
        self.layout = layout
        self.n = len(self.data["X"])
        # Keep only the requested SNR range. The label of a low-SNR resource element is decided by
        # the noise, not by the input, so a model cannot fit it however long it trains; restricting
        # the range is how the pipeline is checked against an answer that IS reachable.
        self.order = np.arange(self.n)
        nominal = self.data["snr_nominal"]
        keep = np.ones(self.n, dtype=bool)
        if snr_min is not None:
            keep &= nominal >= snr_min
        if snr_max is not None:
            keep &= nominal <= snr_max
        self.order = self.order[keep]
        self.n = int(self.order.size)
        if max_samples:
            self.n = min(self.n, max_samples)
        self.order = self.order[:self.n]
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
        channel = (data["H"][index][:, :, 0].astype(np.float32)
                   + 1j * data["H"][index][:, :, 1].astype(np.float32))
        label = (D3.label_plane_cond(bits, alloc, dmrs_symbols, modulation) if self.cond
                 else D3.label_plane(bits, alloc, dmrs_symbols, modulation))
        if self.target == "genie":
            label = genie_labels(plane, channel, alloc, dmrs_symbols, modulation, cond=self.cond)
        llr_weights = (D3.loss_weights_cond(alloc, dmrs_symbols, modulation) if self.cond
                       else D3.loss_weights(alloc, dmrs_symbols, modulation))
        # The channel head is only constrained where the allocation is, and the target is already
        # divided by the reference amplitude, which is what the head produces.
        subcarriers = np.flatnonzero(D3.allocated_subcarriers(alloc))
        ch_weights = np.zeros((D3.NOF_SUBCARRIERS, NOF_SYMBOLS), dtype=np.float32)
        ch_weights[subcarriers, :] = 1.0

        sigma2_norm = float(data["sigma2_over_r2"][index])
        with np.errstate(divide="ignore", invalid="ignore"):
            noise_target = L.noise_shape_target(sigma2_norm / (np.abs(channel) ** 2 + 1e-30))
        noise_weights = (ch_weights > 0) & noise_target[1]

        if self.layout == "spatial":
            # The contract's current input: the symbol axis is spatial, so the 3x3 mixes time.
            plane_in = D3.respaciate(plane)
        elif self.layout == "probe8":
            plane_in = unfold_plane(plane, alloc, dmrs_symbols)
        else:
            plane_in = plane[:, None, :]
        if self.cond:
            # The Qm CONDITIONS the model as four constant channels: the trunk and the reporting heads are
            # shared across modulations, so the network has to be told which one this reception is (design
            # section 19.102). Appending them here keeps every existing dataset usable unchanged.
            one_hot = D3.qm_one_hot(D3.BITS_PER_SYMBOL[modulation])
            plane_in = np.concatenate(
                [plane_in, np.broadcast_to(one_hot, plane_in.shape[:-1] + (one_hot.size,))], axis=-1)
        return {
            "index": index,
            "x": plane_in,
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
            # The scale is model-external per the contract and is PER MODULATION (design section 19.102): a
            # conditioned batch holds several modulations at once, so the loss cannot use one number for all of
            # them -- QPSK's limit is 24 and the others' is 20, and a batch scaled with the wrong one trains the
            # head against the wrong quantiser.
            "scale": np.float32(D3.RANGE_LIMIT_FLOAT[modulation]),
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


def unfold_plane(plane, alloc, dmrs_symbols):
    """Rearrange the contract's plane into a layout whose TIME axis is a spatial axis.

    The contract folds the fourteen symbols into the channel axis, so a 3x3 kernel can only mix
    frequency; every per-symbol relation has to be learned by 1x1 channel mixing. This layout gives
    each symbol its own spatial slice instead, with eight channels each:

        [Re(y), Im(y), DM-RS mask, allocation, DC, Re(LS), Im(LS), reserved]

    The allocation and DC channels are properties of the subcarrier and are broadcast over symbols,
    and the metadata scalars are dropped: they are derivable from the masks, and this arm exists to
    test the locality, not to be a deployment candidate.
    """
    unfolded = np.zeros((plane.shape[0], NOF_SYMBOLS, 8), dtype=np.float32)
    for symbol in range(NOF_SYMBOLS):
        unfolded[:, symbol, 0] = plane[:, 2 * symbol]
        unfolded[:, symbol, 1] = plane[:, 2 * symbol + 1]
        unfolded[:, symbol, 2] = plane[:, 28 + symbol]
        unfolded[:, symbol, 3] = plane[:, 50]
        unfolded[:, symbol, 4] = plane[:, 51]
    for slot, symbol in enumerate(sorted(dmrs_symbols)[:D3.NOF_LS_SLOTS]):
        unfolded[:, symbol, 5] = plane[:, 42 + 2 * slot]
        unfolded[:, symbol, 6] = plane[:, 43 + 2 * slot]
    return unfolded


def genie_labels(plane, channel, alloc, dmrs_symbols, modulation, cond=False):
    """The bits a receiver with the TRUE channel would decide, in the contract's label layout.

    Equalize each data resource element by the true channel, take the nearest constellation point
    and read its bits off. Nothing here needs information from another resource element, so a model
    that cannot fit THIS is blocked by its per-position capacity rather than by interpolation across
    frequency; a model that fits it but not the coded bits is blocked by the estimation step.
    """
    import eval_depth3 as E
    qm = D3.BITS_PER_SYMBOL[modulation]
    # The constellation is the same for every sample; rebuilding it per call dominated the cost.
    if not hasattr(genie_labels, "points"):
        genie_labels.points, genie_labels.labels = E.constellation(qm)
    grid = D3.unpack_input(plane, alloc, dmrs_symbols, 1)["grid"]
    subcarriers, symbols = D3.data_re_indices(alloc, dmrs_symbols)
    equalized = grid[symbols, subcarriers] / channel[subcarriers, symbols]
    nearest = np.argmin(np.abs(equalized[:, None] - genie_labels.points[None, :]), axis=1)
    bits = genie_labels.labels[nearest].reshape(-1)
    # The layout follows the MODEL's, not the modulation's: a conditioned head strides by COND_STRIDE for every
    # reception, so a genie target scattered with the per-modulation stride would put the bits in channels the
    # head does not label -- and the loss would quietly train on a plane whose non-zero entries moved.
    stride = D3.COND_STRIDE if cond else qm
    plane_out = np.zeros((D3.NOF_SUBCARRIERS, D3.NOF_SYMBOLS * stride), dtype=np.float32)
    resource_element = np.arange(bits.size) // qm
    bit_index = np.arange(bits.size) % qm
    plane_out[subcarriers[resource_element], symbols[resource_element] * stride + bit_index] = 1.0 - bits
    return plane_out


def rename_outputs(mlmodel, qm, keras_names):
    """Rename a converted model's outputs to the contract's names.

    The TensorFlow frontend names the outputs `Identity`, `Identity_1`, ... and loses the Keras
    layer names, while the contract binds them BY NAME. Renaming the spec is what makes the
    artifact self-describing, and it is checked downstream: `depth3_engine_check` refuses a bundle
    whose heads are not named `llr`, `post_eq_noise`, `ch_est` and `dmrs_noise`.

    The heads are identified by the ORDER OF THE KERAS MODEL'S OWN OUTPUT NAMES, which the caller
    passes in, and each one's shape is then CHECKED against the contract. Two simpler rules were
    tried and both are wrong. Matching by channel count alone assumes the counts are unique: the LLR
    head carries `14 * Qm` channels and the channel head `2 * 14`, so at QPSK both are 28 and the
    map collapses from four entries to three -- which surfaced as "the model has 4 outputs, the
    contract has 3" and refused the export, while at 16QAM (56), 64QAM (84) and 256QAM (112) the
    counts differ and the bug stayed hidden. Matching by a hardcoded order assumes the frontend
    emits the heads in the sequence they were built, and it does not: `Identity_1` turned out to
    carry one channel, which is the DM-RS noise head. Taking the names from the model removes both
    assumptions, and checking every shape keeps a mismatch detectable rather than silently bound to
    the wrong head -- binding the channel estimate to the LLR head would produce a receiver that
    decodes garbage from an artifact that looks correct.
    """
    import coremltools as ct
    spec = mlmodel.get_spec()
    # The Keras names the heads are known by, and the shape each must have in the contract. `llr` is
    # per resource element (14 * Qm), the channel estimate is complex (2 * 14), and the two noise
    # heads carry one value per symbol and one for the whole slot.
    contract = {"llr_flat": ("llr", NOF_SYMBOLS * qm), "noise_flat": ("post_eq_noise", NOF_SYMBOLS),
                "ch_est_flat": ("ch_est", 2 * NOF_SYMBOLS), "dmrs_noise": ("dmrs_noise", 1)}
    actual = list(spec.description.output)
    if len(actual) != len(keras_names) or len(actual) != len(contract):
        raise ValueError(f"the model has {len(actual)} outputs, the caller named {len(keras_names)}, "
                         f"the contract has {len(contract)}")
    for output, keras_name in zip(actual, keras_names):
        name, channels = contract.get(keras_name, (None, None))
        if name is None:
            raise ValueError(f"the model's output {keras_name} is not one of the contract's heads")
        got = output.type.multiArrayType.shape[-1]
        if got != channels:
            raise ValueError(f"output {output.name} ({keras_name}) carries {got} channels where the "
                             f"contract's {name} head has {channels}")
        ct.utils.rename_feature(spec, output.name, name)
    return ct.models.MLModel(spec, weights_dir=mlmodel.weights_dir)


def export_deployment(model, qm, path, cond=False, snr_range=None, sinr_cal=None):
    """Convert a TRAINED model to the fp16 deployment graph, with the contract's output names.

    The conversion decides the deployment precision, which is why training runs in fp32: the
    artifact is fp16 either way. The heads are renamed because the TensorFlow frontend emits
    `Identity`, `Identity_1`, ... and drops the Keras layer names, while the contract binds them BY
    NAME; `depth3_engine_check` refuses a bundle whose heads are not named `llr`, `post_eq_noise`,
    `ch_est` and `dmrs_noise`, and a bundle exported before training would carry random weights and
    still pass every shape check.
    """
    import coremltools as ct
    # The input shape is taken from the model rather than from the folded contract: the spatialised
    # trunk has a different one, and a hardcoded shape produces a bundle that converts cleanly and
    # then fails at the binding point, which is the one place the check has to hold.
    shape = [int(d) for d in model.inputs[0].shape[1:]]
    converted = ct.convert(
        model,
        inputs=[ct.TensorType(name="x", shape=(1, *shape), dtype=np.float16)],
        compute_precision=ct.precision.FLOAT16,
        minimum_deployment_target=ct.target.iOS17,
        convert_to="mlprogram")
    converted = rename_outputs(converted, qm, list(model.output_names))
    # EVERY metadata write goes through a SPEC and is REBUILT into a model, because `get_spec()` hands back a COPY:
    # mutating the returned spec and then saving the MODEL silently drops the write -- which is exactly what
    # happened on the first export of a conditioned bundle (the metadata came out holding only coremltools' own
    # keys, and the engine would have refused the bundle it declares). `rename_outputs` above already uses this
    # pattern for the head names.
    # `cond` is a PARAMETER and not the model's attribute: the export-only path loads the `.keras` from disk, and
    # an attribute set at build time does not survive that -- measured 2026-10-16, when an export-only smoke run
    # wrote a conditioned bundle with no declaration at all, which the engine would then have refused.
    # The SNR RANGE the reporting heads were trained on, declared by the bundle itself.
    #
    # WHY IT IS HARD-WIRED AND NOT MEASURED AT RUN TIME: a network does not extrapolate. Measured 2026-10-16 on a
    # model trained on a 20-32 dB corpus, replayed over prescribed-SNR grids: its published SINR read 26.4 dB at
    # 0 dB, 6 dB and 24 dB alike, because every one of those inputs is outside what it ever saw -- while the
    # classical estimator tracked the prescribed SNR to within 0.4 dB. The chain must therefore be able to ask the
    # bundle where its measurements MEAN something, and the only honest source for that answer is the training set.
    if snr_range is not None:
        spec = converted.get_spec()
        spec.description.metadata.userDefined["depth3_snr_range"] = f"{snr_range[0]:.2f},{snr_range[1]:.2f}"
        converted = ct.models.MLModel(spec, weights_dir=converted.weights_dir)
    if sinr_cal:
        # THE PUBLISHED SINR'S CALIBRATION, declared by the artifact that needs it (design section 19.172). The arm's
        # noise head estimates a post-equalisation noise whose scale does not coincide with the classical chain's, so
        # the scheduler was reading a SINR 1.2-3.4 dB low -- in the DEFAULT reporting mode as well, because the arm
        # publishes its own post-equalisation noise in either mode. The correction is affine IN DB
        # (`slope * published + offset`) and per modulation, which is what a slope error needs; it is measured
        # against the classical chain's publication on the corpus and validated on held-out captures, and it never
        # touches the LLR path (the main head is scaled by S_llr, not by this map).
        spec = converted.get_spec()
        spec.description.metadata.userDefined["depth3_sinr_cal"] = ";".join(
            f"{qm_}:{sinr_cal[qm_][0]:.4f},{sinr_cal[qm_][1]:.4f}" for qm_ in sorted(sinr_cal))
        converted = ct.models.MLModel(spec, weights_dir=converted.weights_dir)
    if cond or getattr(model, "_depth3_cond", False):
        spec = converted.get_spec()
        spec.description.metadata.userDefined["depth3_layout"] = "modcond"
        spec.description.metadata.userDefined["depth3_qm_set"] = ",".join(str(qm_) for qm_ in D3.COND_QM_ORDER)
        spec.description.metadata.userDefined["depth3_head_stride"] = str(D3.COND_STRIDE)
        # The ranges are written through a spec each time because `get_spec()` copies (see the note above): two
        # writes to two different copies would keep only the last one.
        if snr_range is not None:
            spec.description.metadata.userDefined["depth3_snr_range"] = f"{snr_range[0]:.2f},{snr_range[1]:.2f}"
        converted = ct.models.MLModel(spec, weights_dir=converted.weights_dir)
    # A conditioned bundle and a per-modulation 256QAM bundle have the same main-head width (14 * 8 = 112
    # channels), so a shape-based check cannot tell them apart -- and they are not interchangeable: one serves
    # every modulation, the other one. The bundle therefore SAYS which contract it follows (written above, through
    # the spec), and the engine reads the declaration rather than inferring a layout from a shared width.
    # THE GEOMETRY, declared by the bundle for the same reason the layout is: this arm assembles a FIXED-WIDTH plane
    # (612 subcarriers), so a bundle exported for another width cannot serve this build -- the runtime would reject
    # its input on every single reception, which reads as "the model is broken" rather than "the model is for another
    # cell". Declared FROM the exported shape, so the declaration cannot drift from the graph (design section 19.162).
    spec = converted.get_spec()
    spec.description.metadata.userDefined["depth3_nsc"] = str(shape[0])
    converted = ct.models.MLModel(spec, weights_dir=converted.weights_dir)
    converted.save(path)
    print(f"wrote the fp16 deployment graph to {path} (input {[1, *shape]}, depth3_nsc={shape[0]})")


def cli_sinr_cal(args):
    """{"bits_per_symbol": (slope, offset)} from `--sinr-cal`, or None."""
    if not args.sinr_cal:
        return None
    out = {}
    for entry in args.sinr_cal.split(";"):
        if not entry.strip():
            continue
        qm_text, _, cal_text = entry.partition(":")
        slope_text, _, offset_text = cal_text.partition(",")
        qm = int(qm_text)
        if qm not in D3.COND_QM_ORDER:
            raise SystemExit(f"--sinr-cal: {qm} is not one of the declared modulations {D3.COND_QM_ORDER}")
        slope, offset = float(slope_text), float(offset_text)
        # A SANITY BAND, because this string is applied to the PUBLISHED SINR (a positive number in dB) and a
        # coefficient pair fitted in the NOISE space (negative dB) has its offset negated on the way here. Measured
        # 2026-10-18: declaring the noise-space offset as-is turned a reception published at 17.07 dB into 20.83
        # where it belonged at 18.20; the scheduler believed it, granted 64QAM for the UE's signalling, and attach
        # failed on every leg that carried the declaration. The band cannot tell right from wrong by itself, so the
        # transform is also PRINTED example by example below.
        if not (0.0 < slope <= 3.0):
            raise SystemExit(f"--sinr-cal: slope {slope} for Qm={qm} is outside (0, 3]; a slope is a dB-for-dB "
                             f"correction and cannot be zero, negative or large")
        if abs(offset) > 20.0:
            raise SystemExit(f"--sinr-cal: offset {offset:+.3f} dB for Qm={qm} is larger than 20 dB; the published "
                             f"SINR would move into a range nothing validated")
        out[qm] = (slope, offset)
        print(f"sinr-cal: the published SINR is corrected as {slope:.4f}*x {offset:+.4f}, i.e. "
              f"10 dB -> {slope * 10.0 + offset:.2f} dB, 20 dB -> {slope * 20.0 + offset:.2f} dB, "
              f"25 dB -> {slope * 25.0 + offset:.2f} dB (Qm={qm}; it multiplies the SINR, not the noise)")
    return out


def cli_snr_range(args):
    """The `--snr-range` knob as a pair, or None. Shared by the two export paths so they cannot disagree."""
    if not args.snr_range:
        return None
    low, high = (float(value) for value in args.snr_range.split(","))
    if high <= low:
        raise SystemExit(f"--snr-range {args.snr_range}: the upper bound is not above the lower one")
    return (low, high)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--train", default="", help="training dataset; not needed with --export-only")
    parser.add_argument("--val", default="", help="validation dataset; not needed with --export-only")
    parser.add_argument("--out", default="", help="run directory; not needed with --export-only")
    parser.add_argument("--modulation", default="256QAM")
    parser.add_argument("--modcond", action="store_true",
                        help="train the MODULATION-CONDITIONED model (design section 19.102): one bundle for "
                             "every modulation, with the Qm as four one-hot input channels and a main head "
                             "whose stride is COND_STRIDE. The dataset must carry mixed modulations (its `qm` "
                             "column already does) and --modulation only picks the sample's DEFAULT where the "
                             "file has none.")
    parser.add_argument("--snr-range", default="",
                        help="the SNR range (dB, 'min,max') the reporting heads were trained on, DECLARED in the "
                             "bundle as depth3_snr_range so the chain can tell a measurement the model is "
                             "entitled to make from one it extrapolated. Empty means: derive it from the training "
                             "set's own snr_nominal percentiles (1st to 99th) when training, and declare nothing "
                             "with --export-only -- a bundle that declares no range cannot be gated, and the "
                             "receiver says so once per arm.")
    parser.add_argument("--sinr-cal", default="",
                        help="affine calibration of the PUBLISHED SINR, per modulation, in dB: "
                             "'2:slope,offset;4:...;6:...;8:...'. Measured against the classical chain's own "
                             "publication (design section 19.172); it corrects the reporting path only.")
    parser.add_argument("--blocks", type=int, default=18)
    parser.add_argument("--layout", choices=("folded", "spatial", "probe8", "unfolded"),
                        default="spatial",
                        help="'folded' is the superseded 64-channel contract, kept so the recorded "
                             "comparisons stay reproducible. 'spatial' is the contract now: the "
                             "symbol axis is a spatial axis, 12 channels per symbol. 'probe8' is "
                             "the R4.4 diagnostic arm that isolated the locality (8 channels); "
                             "'unfolded' is its old name and still works")
    parser.add_argument("--dense-blocks", action="store_true",
                        help="dense 3x3 instead of depthwise. Section 25.3 measured the same "
                             "latency for 5.7x the FLOPs, and depthwise is what caps the "
                             "per-position capacity the diagnosis points at")
    parser.add_argument("--in-channels", type=int, default=0,
                        help="input channels of the unfolded trunk. 0 takes it from the batches, "
                             "which is the only value that cannot disagree with the data; the "
                             "export-only path has no batches and passes it explicitly")
    parser.add_argument("--c-hid", type=int, default=64,
                        help="trunk width. The contract freezes the tensor shapes, not the width "
                             "inside the network, and a per-position decision over a 256QAM "
                             "constellation needs more capacity than 64 channels gave it")
    parser.add_argument("--epochs", type=int, default=150)
    parser.add_argument("--batch", type=int, default=8)
    parser.add_argument("--lr", type=float, default=1e-3)
    parser.add_argument("--max-train", type=int, default=0, help="cap the training samples")
    parser.add_argument("--max-val", type=int, default=64)
    parser.add_argument("--target", choices=("coded", "genie"), default="coded",
                        help="'coded' is the contract's task. 'genie' replaces the label with the "
                             "hard decisions a receiver with the true channel would make, which "
                             "isolates per-position capacity from the estimation step")
    parser.add_argument("--snr-min", type=float, default=None,
                        help="keep only samples at or above this nominal SNR. A low-SNR label is "
                             "decided by the noise, so training on it cannot converge to a low "
                             "error rate; the high-SNR slice is the one with a reachable answer")
    parser.add_argument("--snr-max", type=float, default=None)
    parser.add_argument("--w-llr", type=float, default=L.DEFAULT_WEIGHTS["llr"])
    parser.add_argument("--w-ch", type=float, default=L.DEFAULT_WEIGHTS["ch_est"])
    parser.add_argument("--w-noise", type=float, default=L.DEFAULT_WEIGHTS["post_eq_noise"])
    parser.add_argument("--no-aux", action="store_true", help="LLR head only, for the R2.4 ablation")
    parser.add_argument("--llr-linear", action="store_true",
                        help="make the LLR head linear and bound it by clamping in the loss, "
                             "instead of a tanh whose gradient vanishes where confident bits live")
    parser.add_argument("--scale-init", type=float, default=1.0,
                        help="initial gain of the residual LayerScale. The refiner uses 0.1 because "
                             "it learns a correction on top of its input; this network learns the "
                             "whole mapping from scratch, and at 0.1 the eighteen blocks each shrink "
                             "their branch tenfold, so training stalls")
    parser.add_argument("--no-fold-scale", action="store_true",
                        help="keep the LayerScale as a separate multiply. Costs about 200 us on "
                             "the ANE (section 45.8); the default folds it into the convolution")
    parser.add_argument("--init", default="",
                        help="start from these weights instead of a fresh initialisation. A run "
                             "whose curve has not flattened can then be EXTENDED rather than "
                             "guessed at, which is what makes the epoch count a measurement")
    parser.add_argument("--export-only", default="",
                        help="export this trained .keras to the fp16 deployment graph and exit")
    parser.add_argument("--export-probe", default="",
                        help="also write the graph as a CoreML mlpackage, so the ANE probe "
                             "measures the artifact this script trains rather than a copy of it")
    parser.add_argument("--probe-graph", default="",
                        help="build the graph from the architecture flags, export it and EXIT "
                             "without training, so ANE placement and latency can be measured "
                             "before a training run is paid for. The weights are random: the "
                             "artifact is for measurement and must never receive")
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
    if args.layout == "unfolded":
        # The name the R4.4 runs were recorded under. Kept so their command lines still run.
        print("--layout unfolded is the old name of probe8")
        args.layout = "probe8"

    if args.export_only:
        exported = tf.keras.models.load_model(
            args.export_only, compile=False,
            custom_objects={"FoldedScaleConv": FoldedScaleConv})
        if not args.export_probe:
            parser.error("--export-only needs --export-probe")
        # The range has to come from the knob on this path: --export-only loads a `.keras` and no dataset, so
        # there are no `snr_nominal` percentiles to derive it from (see the note where training derives it).
        export_deployment(exported, D3.BITS_PER_SYMBOL[args.modulation], args.export_probe,
                          cond=args.modcond, snr_range=cli_snr_range(args), sinr_cal=cli_sinr_cal(args))
        return 0

    qm = D3.BITS_PER_SYMBOL[args.modulation]
    # The channel count follows the packing the layout names, so the trunk cannot be built for a
    # layout the batches do not produce. The batches are then checked against it rather than
    # believed: a `--layout` that disagrees with the data is the kind of error that trains happily
    # and deploys into a shape the seam cannot fill.
    in_channels = args.in_channels or {"folded": D3.NOF_CHANNELS, "probe8": 8}.get(
        args.layout, D3.NOF_CHANNELS_SPATIAL)
    if args.modcond:
        # The conditioning is FOUR EXTRA INPUT CHANNELS, appended by the dataset at sample time (it is the sample
        # that knows its Qm). The model therefore takes the data's channel count, and this is the one place that
        # adds them -- build() must not widen again, and the check below compares the two numbers.
        in_channels += len(D3.COND_QM_ORDER)

    if args.probe_graph:
        # Architecture -> ANE latency, before paying for a training run. The weights are random and
        # the artifact must never reach a receiver: it exists to be measured. `--in-channels` and
        # the architecture flags are the whole input, so no dataset is needed.
        print(f"probe graph: layout {args.layout}, {in_channels} input channels, "
              f"c_hid {args.c_hid}, {args.blocks} blocks -- RANDOM WEIGHTS, measure only")
        probe_model = build(blocks=args.blocks, c_hid=args.c_hid, qm=qm, aux_heads=not args.no_aux,
                            residual=not args.no_residual, dtype=args.dtype,
                            fold_scale=not args.no_fold_scale, scale_init=args.scale_init,
                            llr_linear=args.llr_linear, dense=args.dense_blocks,
                            layout=args.layout, in_channels=in_channels, cond=args.modcond)
        export_deployment(probe_model, qm, args.probe_graph, cond=args.modcond)
        return 0

    if not args.train or not args.val or not args.out:
        parser.error("--train, --val and --out are required unless --export-only or --probe-graph "
                     "is given")
    train = Batches(args.train, args.modulation, args.max_train, seed=args.seed,
                    snr_min=args.snr_min, snr_max=args.snr_max, target=args.target,
                    layout=args.layout, cond=args.modcond)
    val = Batches(args.val, args.modulation, args.max_val, seed=args.seed + 1,
                  snr_min=args.snr_min, snr_max=args.snr_max, target=args.target,
                  layout=args.layout, cond=args.modcond)
    qm = train.qm
    if args.modcond:
        # The head serves every modulation, so its stride is the widest one; the per-sample Qm travels as the
        # conditioning channels and as the label mask. This is the whole difference from a per-modulation run.
        print(f"train {train.n} samples, val {val.n} samples, MODULATION-CONDITIONED "
              f"(head stride {D3.COND_STRIDE}, {len(D3.COND_QM_ORDER)} conditioning channels, "
              f"modulations from the dataset)")
    else:
        print(f"train {train.n} samples, val {val.n} samples, {args.modulation} (Qm={qm})")

    from_data = int(train.sample(0)["x"].shape[-1])
    if from_data != in_channels:
        parser.error(f"--layout {args.layout} builds {in_channels} input channels but the data "
                     f"carries {from_data}")
    print(f"layout {args.layout}, {in_channels} input channels")
    model = build(blocks=args.blocks, c_hid=args.c_hid, qm=qm, aux_heads=not args.no_aux,
                  residual=not args.no_residual, dtype=args.dtype,
                  fold_scale=not args.no_fold_scale, scale_init=args.scale_init,
                  llr_linear=args.llr_linear, dense=args.dense_blocks, layout=args.layout,
                  in_channels=in_channels, cond=args.modcond)
    parameters = int(sum(np.prod(w.shape) for w in model.weights))
    print(f"parameters {parameters}")
    if args.init:
        model.load_weights(args.init)
        print(f"started from {args.init}")

    # The scale is model-external per the contract, so the loss sees the LLR the quantizer sees. It is carried
    # PER SAMPLE (see Batches.sample): a conditioned batch mixes modulations whose limits differ.
    scale = D3.RANGE_LIMIT_FLOAT[args.modulation]
    # The width of the main head follows the CONTRACT in use, not the default modulation: a conditioned model
    # always carries COND_STRIDE per symbol (design section 19.102).
    head_channels = NOF_SYMBOLS * (D3.COND_STRIDE if args.modcond else qm)
    optimizer = tf.keras.optimizers.Adam(args.lr)
    weights = {"llr": args.w_llr, "ch_est": args.w_ch, "post_eq_noise": args.w_noise,
               "dmrs_noise": args.w_noise}

    @tf.function
    def step(x, targets, training):
        with tf.GradientTape() as tape:
            outputs = model(x, training=training)
            # Every head leaves the model with the contract's `[width, channels]` tail, so one
            # reshape reads each of them and the loss cannot drift with the trunk's layout: the
            # spatial axes are an implementation detail that stops at the head.
            llr = tf.reshape(tf.cast(outputs["llr"], tf.float32),
                             (-1, D3.NOF_SUBCARRIERS, head_channels)) * targets["scale"][:, None, None]
            if args.llr_linear:
                # The bound the contract asks for, applied where it is differentiable, per sample.
                llr = tf.clip_by_value(llr, -targets["scale"][:, None, None], targets["scale"][:, None, None])
            terms = {"llr": L.tf_bce_loss(llr, targets["llr_shape"], targets["llr_weights"])}
            if not args.no_aux:
                ch = tf.reshape(tf.cast(outputs["ch_est"], tf.float32),
                                (-1, D3.NOF_SUBCARRIERS, 2 * NOF_SYMBOLS))
                terms["ch_est"] = L.tf_nmse(
                    tf.complex(ch[:, :, 0::2], ch[:, :, 1::2]),
                    tf.complex(targets["ch"][:, :, :, 0], targets["ch"][:, :, :, 1]),
                    targets["ch_weights"])
                noise = tf.reshape(tf.cast(outputs["post_eq_noise"], tf.float32),
                                   (-1, D3.NOF_SUBCARRIERS, NOF_SYMBOLS))
                error = (noise - targets["noise"]) ** 2
                terms["post_eq_noise"] = (tf.reduce_sum(error * targets["noise_weights"])
                                          / tf.maximum(tf.reduce_sum(targets["noise_weights"]), 1.0))
                dmrs = tf.reshape(tf.cast(outputs["dmrs_noise"], tf.float32), (-1,))
                terms["dmrs_noise"] = tf.reduce_mean((dmrs - targets["dmrs_noise"]) ** 2)
            total = sum(weights[name] * terms[name] for name in terms)
        if training:
            gradients = tape.gradient(total, model.trainable_variables)
            optimizer.apply_gradients(zip(gradients, model.trainable_variables))
        return total, terms, llr

    # The gather rule follows the MODEL: a conditioned head carries every modulation in one stride-8 layout, so
    # the LLRs it produces must be gathered with that layout rather than with the reception's own Qm.
    GATHER = D3.llr_gather_cond if getattr(val, "cond", False) else D3.llr_gather

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
                "scale": tf.constant(stack(samples, "scale")),
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
            bler.add(index, GATHER(plane, val.allocation_of(index),
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
                bler.add(index, GATHER(perfect_llr(samples[index_sample]),
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
                    gathered = GATHER(llr.numpy()[row], val.allocation_of(index),
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

    # THE DECLARED RANGE. From the knob when given, otherwise from the training set itself: the 1st to 99th
    # percentile of `snr_nominal`, which is the range the reporting heads actually saw. The extremes are left out
    # on purpose -- a handful of samples at the very edge are not a domain, and declaring them would widen the
    # gate to receptions the model has essentially never met.
    snr_range = cli_snr_range(args)
    if snr_range is None and args.train:
        nominal = np.load(args.train, allow_pickle=True)["snr_nominal"]
        # ONLY THE FINITE ENTRIES: one NaN poisons the percentile, and a bundle that declares 'nan,nan' is read by
        # the engine as "no range declared" -- the domain gate would be silently off (measured 2026-10-17, on the
        # first export of the final checkpoint).
        nominal = nominal[np.isfinite(nominal)]
        snr_range = (float(np.percentile(nominal, 1)), float(np.percentile(nominal, 99)))
        print(f"declared SNR range from the training set: {snr_range[0]:.2f} to {snr_range[1]:.2f} dB "
              f"(1st to 99th percentile of {nominal.size} sample(s))")

    # Export AFTER training, from the trained weights. Exporting first would produce a deployment
    # graph with random weights that passes every shape check, which is the kind of artifact that
    # reaches a receiver and produces plausible nonsense.
    if args.export_probe:
        model.load_weights(os.path.join(args.out, "depth3_best.keras"))
        export_deployment(model, qm, args.export_probe, cond=args.modcond, snr_range=snr_range,
                          sinr_cal=cli_sinr_cal(args))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
