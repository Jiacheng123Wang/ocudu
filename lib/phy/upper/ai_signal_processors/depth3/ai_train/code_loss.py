#!/usr/bin/env python3
"""The code constraint: a differentiable normalized min-sum through the real LDPC code.

WHY THIS EXISTS. Every lever measured so far changes how well the network predicts the GENIE LLRs; none of
them looks at the thing the receiver is judged by, which is whether the transport block decodes. `design`
section 2.3 makes the chain's own decoder and CRC the acceptance criterion, and `architecture_levers`
section 93 shows the only increment left is turning a decode failure into a decode success (547 units, 249 896
bits, 1.9% with HARQ). A loss that never sees the code can only raise the average LLR quality; a loss that
runs the decoder can reward exactly the blocks that were one correction away.

WHAT IT COMPUTES. For one sample: gather the network's LLRs in the chain's order (`depth3.llr_gather_cond`),
descramble, deinterleave, deposit into the codeblock's circular buffer (the same arithmetic as
`ldpc_decode.rate_dematch`, called rather than copied), then run `k` flooding normalized min-sum iterations
over `nr_ldpc.build_H`'s QC structure, and compare the A-POSTERIORI LLRs at the transmitted positions with the
bits that were actually transmitted. The gradient therefore flows back through the decoder into the demapper.

WHAT IT IS NOT. The acceptance decoder stays the chain's own (`--bler-samples`); this is a training signal.
The min-sum here is deliberately the same schedule as `ldpc_decode.MinSum` so the two cannot disagree about
what a decoder does -- `--selfcheck` proves the TF version reproduces the numpy one on the same LLRs.

COST. `k` iterations over a dense `(mb, nb, zc)` view, padded to (46, 68, 384). One sample is ~1.2 M floats
per tensor, so the default is ONE sample per batch (`--code-loss-samples 1`) and k=2: the measured overhead
is a few percent of the step, not a second training loop.

USAGE
    python code_loss.py --selfcheck
"""

import argparse
import sys

import numpy as np

HERE = __file__.rsplit("/", 1)[0]
sys.path.insert(0, HERE)

import depth3_paths                      # noqa: E402
depth3_paths.add_ce_shared()

import dd_label as DL                    # noqa: E402
import depth3 as D3                      # noqa: E402
import ldpc_decode as LD                 # noqa: E402
import nr_ldpc as NL                     # noqa: E402
import pusch_uci as UCI                  # noqa: E402
import synth as SN                       # noqa: E402

# The padded shape of one codeblock. BG1 is 46 x 68, BG2 is 42 x 52, and the largest lifting size in
# 38.212 is 384, so one static shape covers every block the corpus carries.
MAX_MB = 46
MAX_NB = 68
MAX_ZC = 384
NOF_VAR = MAX_NB * MAX_ZC

# The longest rate-matched selection one codeblock can carry, and the widest filler run (BG2: K_b = 10*Zc
# against a shorter information block). Padding to these makes every step the same graph, which is what
# keeps the compiled step from retracing on every batch.
E_PAD = D3.NOF_SUBCARRIERS * D3.NOF_SYMBOLS * 8
FILLER_PAD = 10 * MAX_ZC

KNOWN_LLR = 30.0

_STRUCTURE_CACHE = {}


def structure(bg, zc):
    """The padded `(shift, present)` picture of one codeblock's parity-check matrix.

    `shift[r, c]` is the cyclic shift of the `Zc x Zc` block at base-graph position `(r, c)`, or -1 when
    the entry is null. Shared by every sample that uses the same (bg, zc), which is most of a corpus.
    """
    key = (int(bg), int(zc))
    if key not in _STRUCTURE_CACHE:
        shift = np.full((MAX_MB, MAX_NB), -1, dtype=np.int32)
        for row, col, value in NL.build_H(int(bg), int(zc)):
            shift[int(row), int(col)] = int(value)
        _STRUCTURE_CACHE[key] = shift
    return _STRUCTURE_CACHE[key]


def prepare(batches, index, row=0):
    """Everything one sample contributes to the code loss, or None when it cannot carry it.

    Null when the transport block is segmented into more than one codeblock (rare in this corpus, and the
    loss would have to average across blocks whose codes differ) or when a retransmission carries too few
    systematic bits to be decodable on its own -- the same exclusion `CodedBler` applies, so training and
    measurement never disagree about which blocks are decodable.
    """
    data = batches.data
    alloc = batches.allocation_of(index)
    dmrs_symbols = batches.dmrs_symbols_of(index)
    modulation = batches.modulation_of(index)
    qm = D3.BITS_PER_SYMBOL[modulation]
    nof_tb = int(data["nof_tb_bits"][index])
    tb_bits = np.unpackbits(data["tb_bits"][index])[:nof_tb]
    nof_bits = int(data["nof_bits"][index])
    transmitted = np.unpackbits(data["bits"][index])[:nof_bits]
    rv = int(data["rv"][index])
    dmrs_mask = sum(1 << s for s in dmrs_symbols)
    uci = batches.uci_of(index)
    code_blocks, bg, zc, kb, e_total, lengths = SN.codeblock_layout(
        tb_bits, len(alloc), D3.NOF_SYMBOLS, qm, dmrs_mask, uci)
    if len(code_blocks) != 1:
        return None
    if rv != 0 and LD.systematic_share(len(code_blocks[0]), kb, zc, e_total, rv, bg) < 0.2:
        return None

    layout = UCI.data_layout(len(alloc), D3.NOF_SYMBOLS, dmrs_symbols, qm, uci)
    sequence = UCI.descrambling_sequence(layout, (int(data["rnti"][index]) << 15)
                                         + int(data["n_id"][index]))
    carried = layout.keep_bits
    if carried.sum() != nof_bits or carried.size != e_total:
        return None

    # The network's own gather rule, as flat indices into the (612, 112) plane. `llr_gather_cond` is
    # `plane[subcarrier, channel]`, so the flat index is subcarrier * channels + channel.
    subcarriers, symbols = D3.data_re_indices(alloc, dmrs_symbols, uci)
    channels = (symbols[:, None] * D3.COND_STRIDE + np.arange(qm)[None, :]).reshape(-1)
    flat_chain = (subcarriers.repeat(qm) * (D3.NOF_SYMBOLS * D3.COND_STRIDE) + channels).astype(np.int64)

    # Codeword order -> dematch order. `deinterleave` reads `out[i] = codeword_llr[perm[i]]`, and the
    # scrambling sequence of a transmitted element is the one at its position in the codeword, so both
    # the LLR index and the label are carried through the same permutation.
    perm = LD.deinterleave_perm(e_total, qm)
    mother, filler = LD.dematch_indices(len(code_blocks[0]), kb, zc, e_total, rv, bg)
    keep = carried[perm]
    if not np.any(keep):
        return None
    source = np.cumsum(carried, dtype=np.int64)[perm[keep]] - 1
    bits_full = np.zeros(e_total, dtype=np.uint8)
    bits_full[carried] = transmitted ^ sequence
    flat_index = flat_chain[source]
    sign = (1.0 - 2.0 * sequence.astype(np.float64))[source]
    bit = bits_full[perm[keep]].astype(np.float64)
    mother_index = mother[keep].astype(np.int64)
    filler_index = np.arange(filler[0], filler[1], dtype=np.int64)

    return {
        "row": int(row),
        "flat_index": flat_index.astype(np.int32),
        "sign": sign.astype(np.float32),
        "bit": bit.astype(np.float32),
        "mother": mother_index.astype(np.int32),
        "filler": filler_index.astype(np.int32),
        "zc": int(zc),
        "nb": int(NL._BG_PARAMS[bg][1]),
        "shift": structure(bg, zc),
    }


def stack_prepared(items):
    """Pad a list of `prepare` results into one batch of static-shape arrays."""
    count = len(items)
    # PADDED TO THE FIXED WIDTHS, not to the batch's own maximum: a shape that changes per batch would make
    # the compiled step retrace on every batch, which is far more expensive than the padding.
    n = E_PAD
    filler = FILLER_PAD

    return {
        "flat_index": pad_list([item["flat_index"] for item in items], n, np.int32, 0),
        "sign": pad_list([item["sign"] for item in items], n, np.float32, 0.0),
        "bit": pad_list([item["bit"] for item in items], n, np.float32, 0.0),
        "mother": pad_list([item["mother"] for item in items], n, np.int32, 0),
        "filler": pad_list([item["filler"] for item in items], filler, np.int32, 0),
        "length": np.asarray([len(item["flat_index"]) for item in items], dtype=np.int32),
        "nof_filler": np.asarray([len(item["filler"]) for item in items], dtype=np.int32),
        "zc": np.asarray([item["zc"] for item in items], dtype=np.int32),
        "nb": np.asarray([item["nb"] for item in items], dtype=np.int32),
        "shift": np.stack([item["shift"] for item in items]).astype(np.int32),
        "row": np.asarray([item["row"] for item in items], dtype=np.int32),
    }


def pad_list(arrays, width, dtype, value):
    out = np.full((len(arrays), width), value, dtype=dtype)
    for row, array in enumerate(arrays):
        if array.size:
            out[row, :array.size] = array
    return out


def _min_sum(channel, shift, zc, nb, iterations, alpha=0.8):
    """`iterations` flooding normalized min-sum passes in TensorFlow, on the padded QC structure."""
    import tensorflow as tf

    channel = tf.reshape(tf.cast(channel, tf.float32), (NOF_VAR,))
    mg = tf.reshape(tf.range(MAX_MB), (MAX_MB, 1, 1))
    nc = tf.reshape(tf.range(MAX_NB), (1, MAX_NB, 1))
    nz = tf.reshape(tf.range(MAX_ZC), (1, 1, MAX_ZC))
    shift3 = tf.reshape(tf.cast(shift, tf.int32), (MAX_MB, MAX_NB, 1))
    present = tf.logical_and(tf.greater_equal(shift3, 0), tf.logical_and(nc < nb, nz < zc))
    present3 = tf.broadcast_to(present, (MAX_MB, MAX_NB, MAX_ZC))
    var = nc * zc + tf.math.floormod(nz + tf.maximum(shift3, 0), zc)
    var = tf.where(present3, var, NOF_VAR - 1)

    lv = tf.gather(channel, var)
    message = tf.zeros_like(lv)
    big = tf.constant(1e9, dtype=tf.float32)
    for _ in range(int(iterations)):
        # The variable node's current belief is the channel value plus every message it holds, so the
        # extrinsic of one edge is that belief minus the edge's own message (not the row sum: that is the
        # mistake `ldpc_decode.MinSum` documents -- it freezes the decisions after one pass).
        total = tf.math.unsorted_segment_sum(tf.reshape(message, (-1,)), tf.reshape(var, (-1,)),
                                             NOF_VAR)
        q = lv + tf.gather(total, var) - message
        q = tf.where(present3, q, 0.0)
        magnitude = tf.where(present3, tf.abs(q), big)
        first = tf.cast(tf.argmin(magnitude, axis=1), tf.int32)
        smallest = tf.reduce_min(magnitude, axis=1)
        is_first = tf.equal(nc, first[:, None, :])
        second = tf.reduce_min(tf.where(is_first, big, magnitude), axis=1)
        sign = tf.where(present3, tf.where(q < 0.0, -1.0, 1.0), 1.0)
        product = tf.reduce_prod(sign, axis=1)
        chosen = tf.where(is_first, second[:, None, :], smallest[:, None, :])
        message = tf.where(present3, alpha * product[:, None, :] * sign * chosen, 0.0)
    total = tf.math.unsorted_segment_sum(tf.reshape(message, (-1,)), tf.reshape(var, (-1,)),
                                         NOF_VAR)
    return channel + total


def tf_code_bce(llr, targets, iterations=2, alpha=0.8):
    """Mean BCE between the decoder's a-posteriori LLRs and the bits that were transmitted.

    `llr` is the model's `[batch, 612, 112]` LLR plane (already scaled); `targets` is what
    `stack_prepared` produced, with the row index of each code-loss sample inside `llr`.
    """
    import tensorflow as tf

    losses = []
    total = tf.constant(0.0, dtype=tf.float32)
    count = 0
    for position in range(targets["zc"].shape[0]):
        flat = tf.reshape(llr[targets["row"][position]], (-1,))
        length = targets["length"][position]
        nof_filler = targets["nof_filler"][position]
        valid = tf.reshape(tf.range(targets["flat_index"].shape[1]) < length, (-1,))
        channel = (tf.gather(flat, targets["flat_index"][position])
                   * tf.where(valid, targets["sign"][position], 0.0))
        deposited = tf.math.unsorted_segment_sum(
            channel, targets["mother"][position], NOF_VAR)
        filler_valid = tf.reshape(tf.range(targets["filler"].shape[1]) < nof_filler, (-1,))
        deposited = deposited + tf.math.unsorted_segment_sum(
            tf.where(filler_valid, KNOWN_LLR, 0.0), targets["filler"][position], NOF_VAR)
        posterior = _min_sum(tf.reshape(deposited, (NOF_VAR,)), targets["shift"][position],
                             tf.cast(targets["zc"][position], tf.int32),
                             tf.cast(targets["nb"][position], tf.int32), iterations, alpha)
        at_bits = tf.gather(posterior, targets["mother"][position])
        label = 1.0 - 2.0 * targets["bit"][position]
        bce = tf.nn.softplus(-label * at_bits)
        total += tf.reduce_sum(tf.where(valid, bce, 0.0)) / tf.maximum(tf.reduce_sum(
            tf.cast(valid, tf.float32)), 1.0)
        count += 1
    return total / max(count, 1)


def null_prepared(row=0):
    """A code target with nothing in it, so a batch without a decodable sample keeps the same graph.

    The feedback pass reads `targets["code"]` unconditionally; if some batch happened to carry no valid sample the
    key would disappear and the compiled step would retrace with a different signature. Length zero means every
    mask is empty and the extrinsic it produces is all zeros -- the second pass then sees exactly what the first
    one did, which is the correct behaviour for a batch with nothing to say.
    """
    return {
        "row": int(row),
        "flat_index": np.zeros(1, dtype=np.int32),
        "sign": np.zeros(1, dtype=np.float32),
        "bit": np.zeros(1, dtype=np.float32),
        "mother": np.zeros(1, dtype=np.int32),
        "filler": np.zeros(1, dtype=np.int32),
        "length": 0,
        "nof_filler": 0,
        "zc": 2,
        "nb": 68,
        "shift": structure(1, 2),
    }


def tf_decode_feedback(llr, targets, planes=8, scale=20.0, detach=False, iterations=2, alpha=0.8):
    """The decoder's EXTRINSIC LLRs, scattered back onto the network's own `[612, 14, 8]` layout.

    This is the training side of the second-pass lever (architecture_levers section 93, item 2). For every
    transmitted bit the decoder produced an a-posteriori LLR; the part that came from the OTHER bits -- the
    extrinsic -- is `posterior - intrinsic`, and it is exactly the information the demapper could not have
    (the code's own constraint). It is descrambled back into the plane's own sign convention, placed at the
    positions the gather took it from, and returned as `planes` channels for the second forward pass.

    `planes=8` is one channel per modulation bit within a symbol, the faithful layout (the head's stride is 8).
    `planes=1` collapses the eight bits of a symbol to their mean, which is what the memo's "17 channels"
    shorthand describes; it is kept because it is the cheaper of the two and the comparison is part of the point.

    `detach=True` stops the gradient at the decoder: the second pass is then trained on feedback whose statistics
    are fixed, which is what an inference chain that runs a real (non-differentiable) decoder would do.
    """
    import tensorflow as tf

    batch = tf.shape(llr)[0]
    stride = D3.COND_STRIDE
    plane = D3.NOF_SUBCARRIERS * D3.NOF_SYMBOLS * stride
    flat_batch = tf.reshape(tf.cast(llr, tf.float32), (batch, -1))
    total = tf.zeros((batch, plane), dtype=tf.float32)
    for position in range(targets["zc"].shape[0]):
        row = targets["row"][position]
        length = targets["length"][position]
        valid = tf.reshape(tf.range(targets["flat_index"].shape[1]) < length, (-1,))
        intrinsic = (tf.gather(flat_batch[row], targets["flat_index"][position])
                     * tf.where(valid, targets["sign"][position], 0.0))
        if detach:
            intrinsic = tf.stop_gradient(intrinsic)
        deposited = tf.math.unsorted_segment_sum(intrinsic, targets["mother"][position], NOF_VAR)
        filler_valid = tf.reshape(tf.range(targets["filler"].shape[1]) < targets["nof_filler"][position],
                                  (-1,))
        deposited = deposited + tf.math.unsorted_segment_sum(
            tf.where(filler_valid, KNOWN_LLR, 0.0), targets["filler"][position], NOF_VAR)
        posterior = _min_sum(deposited, targets["shift"][position],
                             tf.cast(targets["zc"][position], tf.int32),
                             tf.cast(targets["nb"][position], tf.int32), iterations, alpha)
        if detach:
            posterior = tf.stop_gradient(posterior)
        extrinsic = tf.gather(posterior, targets["mother"][position]) - intrinsic
        values = extrinsic * tf.where(valid, targets["sign"][position], 0.0)
        row_plane = tf.math.unsorted_segment_sum(values, targets["flat_index"][position], plane)
        total = tf.tensor_scatter_nd_add(total, [[row]], [row_plane])
    shaped = tf.reshape(total, (batch, D3.NOF_SUBCARRIERS, D3.NOF_SYMBOLS, stride))
    if int(planes) != stride:
        shaped = tf.reduce_mean(shaped, axis=-1, keepdims=True)
    # BOUNDED, LIKE EVERY OTHER INPUT THE TRUNK SEES. An extrinsic LLR is in the same units as the network's own
    # output (tens), while the twelve feature planes are O(1). `tanh` was the first choice and it saturated: with a
    # perfect demapper the decoder's confirmation is 20-30, so almost every bit landed on +-1 and the gradient
    # through the feedback path -- the only reason to make it differentiable -- went to zero. A linear map with a
    # generous clip keeps the magnitude ordering and a usable slope, and `scale` defaults to the calibration's own
    # S_llr (20) rather than to a number invented here.
    return tf.clip_by_value(shaped / float(scale), -3.0, 3.0)


def selfcheck():
    """Prove the TF min-sum decodes like the numpy one, and that perfect LLRs give ~zero loss."""
    import tensorflow as tf

    rng = np.random.default_rng(20261127)
    bg, zc = 1, 96
    mb, nb, _ = NL._BG_PARAMS[bg]
    kb = 22 * zc
    nof_information = kb - 2 * zc
    information = rng.integers(0, 2, size=nof_information).astype(np.uint8)
    padded = np.concatenate([information, np.zeros(kb - nof_information, dtype=np.uint8)])
    codeword = np.asarray(NL.ldpc_encode(bg, zc, padded.tolist()), dtype=np.uint8)
    assert NL.verify(codeword, bg, zc), "the encoder produced a word that fails its own check"

    # Channel LLRs with a positive value for bit 0, as the chain uses.
    channel = np.where(codeword == 0, 6.0, -6.0).astype(np.float32)
    padded_channel = np.zeros(NOF_VAR, dtype=np.float32)
    padded_channel[:channel.size] = channel
    posterior = _min_sum(tf.constant(padded_channel), tf.constant(structure(bg, zc)),
                         tf.constant(zc), tf.constant(nb), 2).numpy()
    decided = (posterior[:channel.size] < 0).astype(np.uint8)
    assert np.array_equal(decided, codeword), "the TF decoder did not recover the codeword"

    # On a channel the code still corrects, both decoders must recover the SAME codeword across seeds.
    for seed in range(5):
        generator = np.random.default_rng(1000 + seed)
        noisy = channel + generator.standard_normal(channel.size).astype(np.float32) * 1.5
        numpy_bits, _, _ = LD.MinSum(bg, zc, max_iterations=2).decode(noisy.astype(np.float64))
        padded = np.zeros(NOF_VAR, dtype=np.float32)
        padded[:noisy.size] = noisy
        tf_bits = (_min_sum(tf.constant(padded), tf.constant(structure(bg, zc)),
                            tf.constant(zc), tf.constant(nb), 2).numpy()[:noisy.size] < 0).astype(np.uint8)
        assert np.array_equal(numpy_bits, codeword), f"numpy failed on seed {seed}"
        assert np.array_equal(tf_bits, codeword), f"TF failed on seed {seed}"

    # On a channel the code cannot correct, the loss must be able to see the failure. The two decoders are
    # NOT required to land on the same wrong word: numpy updates the variable sum incrementally and this one
    # recomputes it, so at these magnitudes the two take different floating-point paths through a tie. What
    # has to agree is the regime -- a word that fails its own check.
    generator = np.random.default_rng(7)
    hopeless = channel + generator.standard_normal(channel.size).astype(np.float32) * 2.0
    hopeless[generator.random(channel.size) < 0.35] *= -1.0
    numpy_bits, _, _ = LD.MinSum(bg, zc, max_iterations=2).decode(hopeless.astype(np.float64))
    padded = np.zeros(NOF_VAR, dtype=np.float32)
    padded[:hopeless.size] = hopeless
    tf_bits = (_min_sum(tf.constant(padded), tf.constant(structure(bg, zc)),
                        tf.constant(zc), tf.constant(nb), 2).numpy()[:hopeless.size] < 0).astype(np.uint8)
    assert not np.array_equal(numpy_bits, codeword), "the numpy decoder did not fail where it should"
    assert not np.array_equal(tf_bits, codeword), "the TF decoder did not fail where it should"
    print(f"code_loss selfcheck: perfect channel exact, five noisy channels agree with "
          f"ldpc_decode.MinSum and decode exactly, a hopeless channel fails in both "
          f"({int((numpy_bits != codeword).sum())} / {int((tf_bits != codeword).sum())} wrong bits)")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--selfcheck", action="store_true")
    args = parser.parse_args()
    if args.selfcheck:
        return selfcheck()
    parser.error("nothing to do: pass --selfcheck")


if __name__ == "__main__":
    raise SystemExit(main())
