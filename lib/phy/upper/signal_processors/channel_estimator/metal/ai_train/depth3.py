#!/usr/bin/env python3
"""The depth-3 I/O contract in executable form.

The normative table lives in `doc_chinese/llr_ai_detection/model_network_design.md` section 0.2.
This module implements it once so that the dataset builder, the trainer and the seam cannot each
repack 64 channels slightly differently.

Why the contract needs an implementation
----------------------------------------
A contract written only in prose reads as several different contracts, and that is the failure
this workstream already paid for: the network was trained on "the chain's channel estimate
towards the true channel" without anyone checking the input contract, which put the network
behind the classical estimator instead of in front of it (`design` section 12). Section 0.2 is
the corrected contract. `--selfcheck` proves that this implementation and the table agree, by
packing a synthetic capture, unpacking every field and comparing.

The tensor
----------
Input `[1, 612, 1, 64]` fp16, NHWC with the symbol axis folded into the channels:

    channels 0..27   grid I/Q            c = 2t + {0, 1}
    channels 28..41  DM-RS RE mask       c = 28 + t
    channels 42..49  pilot LS (de-boosted), four DM-RS symbol slots, c = 42 + 2k + {0, 1}
    channel 50       allocation mask     a function of the subcarrier
    channel 51       DC marker           a function of the subcarrier
    channels 52..62  metadata scalars, broadcast over the whole plane
    channel 63       reserved, must be zero

Outputs, per modulation:

    llr             [1, 612, 1, 14 * Qm]  c = t * Qm + b, tanh-bounded, scaled outside the model
    post_eq_noise   [1, 612, 1, 14]       c = t, tanh-bounded log10 of the variance
    ch_est          [1, 612, 1, 28]       c = 2t + {0, 1}, linear, divided by the reference r
    dmrs_noise      [1, 1, 1, 1]          tanh-bounded log10 of the DM-RS domain variance

Axis convention: inputs are `[symbol, subcarrier]`, as the chain and the captured files lay them
out. Contract outputs are `[subcarrier, channel]`, as the model's spatial axes require.

Two conventions are silent failure modes, so they are stated here and asserted in `--selfcheck`:

* the LLR is in the SCRAMBLING domain and positive means bit 0 (`log_likelihood_ratio.h:22-25`),
  so the BCE label is `1 - b`;
* the pilot LS is de-boosted by the DM-RS amplitude scaling, which is the convention the chain's
  own estimator uses (`port_channel_estimator_average_impl.cpp:1050`). At two CDM groups without
  data the DM-RS is 3 dB above the data, so the scaling is 1.413 and the raw `y/x` is 3 dB high.
"""

import argparse

import numpy as np

import dmrs as DV

NOF_SYMBOLS = 14
NOF_SUBCARRIERS = 612
NOF_CHANNELS = 64

# Channel map. Slices are half-open and named after the field they carry.
SLICE_GRID = slice(0, 28)
SLICE_DMRS_MASK = slice(28, 42)
SLICE_PILOT_LS = slice(42, 50)
CH_ALLOCATION = 50
CH_DC = 51
SLICE_METADATA = slice(52, 63)
CH_RESERVED = 63

NOF_LS_SLOTS = 4
NOF_DMRS_RE_PER_PRB_TYPE1 = 6

# Metadata channel order, as the contract numbers them.
METADATA_FIELDS = (
    "nof_tx_layers",
    "nof_rx_ports",
    "active_dmrs_ports",
    "nof_cdm_groups_without_data",
    "dmrs_type",
    "nof_dmrs_symbols",
    "nof_symbols_over_14",
    "start_symbol_index_over_14",
    "alloc_nof_prb_over_51",
    "alloc_start_prb_over_51",
    "scaling",
)

BITS_PER_SYMBOL = {"QPSK": 2, "16QAM": 4, "64QAM": 6, "256QAM": 8}

# Input value mapped to LLR_MAX by the chain's quantizer, per modulation
# (`demodulation_mapper_*.cpp`).
RANGE_LIMIT_FLOAT = {"QPSK": 24.0, "16QAM": 20.0, "64QAM": 20.0, "256QAM": 20.0}
LLR_MAX = 120
LLR_INFTY = 127

# `get_sch_to_dmrs_ratio_dB` in `include/ocudu/ran/sch/sch_dmrs_power.h`, indexed by the number of
# CDM groups without data. The DM-RS EPRE rises as data REs are given up to pilots.
SCH_TO_DMRS_RATIO_DB = {1: 0.0, 2: -3.0, 3: -4.77}


def dmrs_amplitude_scaling(nof_cdm_groups_without_data):
    """Amplitude ratio between the DM-RS and the shared channel, as the estimator is configured.

    Matches `pusch_processor_impl.cpp:374`, which passes `convert_dB_to_amplitude(-ratio)`.
    """
    return 10.0 ** (-SCH_TO_DMRS_RATIO_DB[int(nof_cdm_groups_without_data)] / 20.0)


def allocated_subcarriers(alloc_prb):
    """Boolean mask over the 612 subcarriers of the allocation, in absolute indexing."""
    mask = np.zeros(NOF_SUBCARRIERS, dtype=bool)
    for prb in alloc_prb:
        mask[int(prb) * 12:(int(prb) + 1) * 12] = True
    return mask


def pilot_ls(grid, alloc_prb, dmrs_symbols, nslot, nid, nscid, dmrs_type, scaling):
    """De-boosted pilot least-squares estimate on the full grid, zero outside the pilot REs.

    `grid` is `[symbol, subcarrier]` complex. The DM-RS reference symbols come from the module
    verified point by point against the chain's own dump, so the pilots are the one part of the
    input that is known rather than estimated.
    """
    pilots = DV.pilot_subcarriers(sorted(int(r) for r in alloc_prb), dmrs_type=int(dmrs_type))
    ls = np.zeros(grid.shape, dtype=np.complex64)
    for symbol in dmrs_symbols:
        sequence = DV.sequence(int(nslot), int(symbol), int(nid), int(nscid), alloc_prb)
        if sequence.size != pilots.size:
            raise ValueError(f"DM-RS sequence has {sequence.size} values for {pilots.size} pilots")
        ls[symbol, pilots] = grid[symbol, pilots] / (sequence * scaling)
    return ls


def pilot_ls_mask(alloc_prb, dmrs_symbols, dmrs_type):
    """Boolean `[symbol, subcarrier]` mask of the REs `pilot_ls` fills."""
    pilots = DV.pilot_subcarriers(sorted(int(r) for r in alloc_prb), dmrs_type=int(dmrs_type))
    mask = np.zeros((NOF_SYMBOLS, NOF_SUBCARRIERS), dtype=bool)
    for symbol in dmrs_symbols:
        mask[int(symbol), pilots] = True
    return mask


def reference_amplitude(ls, ls_mask):
    """Reference r the field channels are divided by: the RMS of the pilot LS estimates."""
    if ls_mask.sum() == 0:
        return 0.0
    return float(np.sqrt(np.mean(np.abs(ls[ls_mask]) ** 2)))


def pack_input(grid, alloc_prb, dmrs_symbols, meta):
    """Assemble the contract's 64-channel plane from a received grid and its metadata.

    `grid` is `[14, 612]` complex in absolute subcarrier indexing, `meta` carries the
    configuration fields the contract lists, and the DM-RS sequence is regenerated from them.

    Returns `(plane, r)` where `plane` is `[612, 64]` float32 and `r` is the reference amplitude
    the grid and the pilot LS are divided by. The caller restores physical units by multiplying
    the channel-estimate head by `r`; the LLR and the noise heads need no restoration, because
    both are invariant under a common scaling of the grid, the channel and the noise.
    """
    dmrs_symbols = sorted(int(s) for s in dmrs_symbols)
    dmrs_type = int(meta["dmrs_type"])
    scaling = dmrs_amplitude_scaling(meta["nof_cdm_groups_without_data"])

    ls = pilot_ls(grid, alloc_prb, dmrs_symbols, meta["nslot"], meta["n_id"], meta["n_scid"],
                  dmrs_type, scaling)
    ls_mask = pilot_ls_mask(alloc_prb, dmrs_symbols, dmrs_type)
    r = reference_amplitude(ls, ls_mask)
    if not np.isfinite(r) or r <= 0.0:
        raise ValueError(f"reference amplitude r={r} is unusable; the arm must not run")

    plane = np.zeros((NOF_SUBCARRIERS, NOF_CHANNELS), dtype=np.float32)

    # Grid I/Q, symbol t in channels 2t and 2t+1.
    normalized = grid / r
    for symbol in range(NOF_SYMBOLS):
        plane[:, 2 * symbol] = normalized[symbol].real
        plane[:, 2 * symbol + 1] = normalized[symbol].imag

    # DM-RS RE mask, one channel per symbol.
    dmrs_mask = pilot_ls_mask(alloc_prb, dmrs_symbols, dmrs_type)
    for symbol in range(NOF_SYMBOLS):
        plane[:, SLICE_DMRS_MASK.start + symbol] = dmrs_mask[symbol]

    # Pilot LS, one complex pair per DM-RS symbol slot, slots ordered by ascending symbol index.
    for slot, symbol in enumerate(dmrs_symbols):
        if slot >= NOF_LS_SLOTS:
            raise ValueError(f"{len(dmrs_symbols)} DM-RS symbols exceed the {NOF_LS_SLOTS} slots")
        plane[:, SLICE_PILOT_LS.start + 2 * slot] = (ls[symbol] / r).real
        plane[:, SLICE_PILOT_LS.start + 2 * slot + 1] = (ls[symbol] / r).imag

    # Allocation mask and DC marker, functions of the subcarrier only.
    plane[:, CH_ALLOCATION] = allocated_subcarriers(alloc_prb)
    if meta.get("dc_position") is not None:
        plane[int(meta["dc_position"]), CH_DC] = 1.0

    values = (
        float(meta["nof_tx_layers"]),
        float(meta["nof_rx_ports"]),
        float(meta["active_dmrs_ports"]),
        float(meta["nof_cdm_groups_without_data"]),
        float(dmrs_type),
        float(len(dmrs_symbols)),
        float(meta["nof_symbols"]) / 14.0,
        float(meta["start_symbol_index"]) / 14.0,
        float(meta["alloc_nof_prb"]) / 51.0,
        float(min(alloc_prb)) / 51.0,
        float(scaling),
    )
    plane[:, SLICE_METADATA] = np.asarray(values, dtype=np.float32)

    # Channel 63 is reserved and must stay zero, so that a future field can take it without
    # changing the shape. A non-zero value here means some producer invented its own layout.
    plane[:, CH_RESERVED] = 0.0
    return plane, r


def unpack_input(plane, alloc_prb, dmrs_symbols, dmrs_type):
    """Recover every contract field from a packed plane, for checking a producer against it.

    Returns a dict of the fields the table names, with the field values still normalized by the
    reference amplitude. Nothing here is used on the receive path: it exists so that a dataset
    producer can be checked against the contract rather than against another copy of itself.
    """
    dmrs_symbols = sorted(int(s) for s in dmrs_symbols)
    grid = np.zeros((NOF_SYMBOLS, NOF_SUBCARRIERS), dtype=np.complex64)
    for symbol in range(NOF_SYMBOLS):
        grid[symbol] = plane[:, 2 * symbol] + 1j * plane[:, 2 * symbol + 1]

    dmrs_mask = np.zeros((NOF_SYMBOLS, NOF_SUBCARRIERS), dtype=np.float32)
    for symbol in range(NOF_SYMBOLS):
        dmrs_mask[symbol] = plane[:, SLICE_DMRS_MASK.start + symbol]

    ls = np.zeros((NOF_SYMBOLS, NOF_SUBCARRIERS), dtype=np.complex64)
    for slot, symbol in enumerate(dmrs_symbols):
        ls[symbol] = (plane[:, SLICE_PILOT_LS.start + 2 * slot]
                      + 1j * plane[:, SLICE_PILOT_LS.start + 2 * slot + 1])

    metadata = {name: float(plane[0, SLICE_METADATA.start + index])
                for index, name in enumerate(METADATA_FIELDS)}
    return {
        "grid": grid,
        "dmrs_mask": dmrs_mask,
        "pilot_ls": ls,
        "allocation_mask": plane[:, CH_ALLOCATION].copy(),
        "dc_marker": plane[:, CH_DC].copy(),
        "metadata": metadata,
        "reserved": plane[:, CH_RESERVED].copy(),
    }


def llr_channels(modulation):
    """Number of channels the LLR head carries for `modulation`: one symbol block per symbol."""
    return NOF_SYMBOLS * BITS_PER_SYMBOL[modulation]


def llr_channel(symbol, bit, modulation):
    """Channel index of one coded bit: symbol major, bit fastest within the symbol block."""
    return int(symbol) * BITS_PER_SYMBOL[modulation] + int(bit)


def data_re_mask(alloc_prb, dmrs_symbols):
    """Boolean `[subcarrier, symbol]` mask of the REs that carry data.

    A DM-RS symbol carries no data when the CDM groups without data cover the allocation, which
    is the configured case (`nof_cdm_groups_without_data = 2`): its even offsets are pilots and
    its odd offsets are reserved.
    """
    allocated = allocated_subcarriers(alloc_prb)
    mask = np.zeros((NOF_SUBCARRIERS, NOF_SYMBOLS), dtype=bool)
    for symbol in range(NOF_SYMBOLS):
        if symbol in set(int(s) for s in dmrs_symbols):
            continue
        mask[:, symbol] = allocated
    return mask


def llr_valid_mask(alloc_prb, dmrs_symbols, modulation):
    """Boolean `[subcarrier, 14 * Qm]` mask of the LLR channels that carry a coded bit."""
    mask = np.zeros((NOF_SUBCARRIERS, llr_channels(modulation)), dtype=bool)
    data = data_re_mask(alloc_prb, dmrs_symbols)
    qm = BITS_PER_SYMBOL[modulation]
    for symbol in range(NOF_SYMBOLS):
        mask[:, symbol * qm:(symbol + 1) * qm] = data[:, symbol][:, None]
    return mask


def label_plane(bits, alloc_prb, dmrs_symbols, modulation):
    """Scatter the chain's coded bits into the contract's label plane.

    `bits` is the SCRAMBLED coded bit sequence, in the order the chain consumes it: symbol major
    over the non-DM-RS symbols, then ascending allocated subcarrier, then the `Qm` bits of that
    RE with the bit index fastest. This is the order `synth.encode_symbols` produces, because its
    `data_pos` walk is the same walk.

    The plane holds `1 - b`, not `b`: the chain's convention is that a POSITIVE LLR means bit 0,
    so a network trained with `z` as the logit of label `1 - b` has `z` as the LLR directly. A
    plane holding `b` instead would invert every LLR and only look like "it does not train".
    """
    qm = BITS_PER_SYMBOL[modulation]
    expected = int(data_re_mask(alloc_prb, dmrs_symbols).sum()) * qm
    if len(bits) != expected:
        raise ValueError(f"{len(bits)} bits for {expected} data bits in this allocation")
    plane = np.zeros((NOF_SUBCARRIERS, llr_channels(modulation)), dtype=np.float32)
    cursor = 0
    for symbol in range(NOF_SYMBOLS):
        if symbol in set(int(s) for s in dmrs_symbols):
            continue
        for subcarrier in np.flatnonzero(allocated_subcarriers(alloc_prb)):
            for bit in range(qm):
                plane[subcarrier, symbol * qm + bit] = 1.0 - float(bits[cursor])
                cursor += 1
    if cursor != len(bits):
        raise ValueError(f"scattered {cursor} of {len(bits)} bits")
    return plane


def llr_gather(plane, alloc_prb, dmrs_symbols, modulation):
    """Gather a model's LLR plane into the chain's codeword order.

    This is the seam's gather rule. It is the inverse of `label_plane`, and `--selfcheck` asserts
    that, so a model scored through this function is scored in the order the LDPC decoder reads.
    """
    qm = BITS_PER_SYMBOL[modulation]
    out = []
    for symbol in range(NOF_SYMBOLS):
        if symbol in set(int(s) for s in dmrs_symbols):
            continue
        for subcarrier in np.flatnonzero(allocated_subcarriers(alloc_prb)):
            out.extend(plane[subcarrier, symbol * qm:(symbol + 1) * qm])
    return np.asarray(out, dtype=np.float32)


def llr_from_logits(logits, modulation, scale=None):
    """Apply the model-external map: scale the tanh-bounded shape, then quantize to the chain.

    `scale` defaults to the modulation's `range_limit`, which makes the tanh the only saturation
    in the chain, and is refitted after training by the calibration step (`design` section 3.3).
    The result is int8 in `[-LLR_MAX, LLR_MAX]`; `-128` and the reserved `+-LLR_INFTY` are not in
    the quantizer's range, so the arm cannot fabricate a "fixed bit".
    """
    range_limit = RANGE_LIMIT_FLOAT[modulation]
    scale = range_limit if scale is None else float(scale)
    clipped = np.clip(np.asarray(logits, dtype=np.float64) * scale, -range_limit, range_limit)
    return np.rint(clipped / range_limit * LLR_MAX).astype(np.int8)


def loss_weights(alloc_prb, dmrs_symbols, modulation):
    """Per-channel weights for the BCE: one on the data REs, zero everywhere else.

    The DM-RS symbols carry no data, so their `2 * Qm` channels hold no target. Training on them
    would teach the network to predict noise.
    """
    return llr_valid_mask(alloc_prb, dmrs_symbols, modulation).astype(np.float32)


def selfcheck():
    """Prove that this implementation and the section 0.2 table describe the same tensor."""
    rng = np.random.default_rng(20261013)
    alloc_prb = list(range(5, 51))
    dmrs_symbols = [2, 11]
    dmrs_type = 1
    modulation = "256QAM"
    scaling = dmrs_amplitude_scaling(2)
    if abs(scaling - 1.41254) > 1e-4:
        raise AssertionError(f"DM-RS amplitude scaling {scaling} is not the 3 dB table value")

    # A synthetic capture: a random channel, pilots at the contract's positions, unit power data.
    channel = (rng.standard_normal((NOF_SYMBOLS, NOF_SUBCARRIERS))
               + 1j * rng.standard_normal((NOF_SYMBOLS, NOF_SUBCARRIERS))) / np.sqrt(2.0)
    grid = channel.copy()
    pilots = DV.pilot_subcarriers(alloc_prb, dmrs_type=dmrs_type)
    for symbol in dmrs_symbols:
        sequence = DV.sequence(7, symbol, 1, 0, alloc_prb)
        grid[symbol, pilots] = scaling * channel[symbol, pilots] * sequence

    meta = {
        "nslot": 7,
        "n_id": 1,
        "n_scid": 0,
        "dmrs_type": dmrs_type,
        "nof_cdm_groups_without_data": 2,
        "nof_tx_layers": 1,
        "nof_rx_ports": 1,
        "active_dmrs_ports": 1,
        "nof_symbols": 14,
        "start_symbol_index": 0,
        "alloc_nof_prb": len(alloc_prb),
        "dc_position": 306,
    }

    plane, r = pack_input(grid, alloc_prb, dmrs_symbols, meta)

    # The table's shape claims.
    assert plane.shape == (NOF_SUBCARRIERS, NOF_CHANNELS), plane.shape
    assert np.all(plane[:, CH_RESERVED] == 0.0), "the reserved channel is not zero"

    # Every field survives the round trip, so the table has one reading and not several.
    back = unpack_input(plane, alloc_prb, dmrs_symbols, dmrs_type)
    assert np.allclose(back["grid"], grid / r, atol=1e-6), "grid does not round trip"
    assert np.allclose(back["pilot_ls"], pilot_ls(grid, alloc_prb, dmrs_symbols, 7, 1, 0,
                                                 dmrs_type, scaling) / r, atol=1e-6), "LS"
    assert np.array_equal(back["dmrs_mask"].astype(bool),
                          pilot_ls_mask(alloc_prb, dmrs_symbols, dmrs_type)), "DM-RS mask"
    assert np.array_equal(back["allocation_mask"].astype(bool),
                          allocated_subcarriers(alloc_prb)), "allocation mask"
    assert back["dc_marker"][306] == 1.0 and back["dc_marker"].sum() == 1.0, "DC marker"
    assert back["metadata"]["scaling"] == np.float32(scaling), "scaling metadata"
    assert back["metadata"]["nof_dmrs_symbols"] == 2.0, "nof_dmrs_symbols metadata"
    assert back["metadata"]["alloc_nof_prb_over_51"] == np.float32(46 / 51), "alloc width"

    # The de-boosted pilot LS must recover the channel the pilots were built from. This is the
    # check that catches a wrong scaling direction: the raw y/x is 3 dB high, and 3 dB is small
    # enough to look like a training problem rather than a convention error.
    ls = pilot_ls(grid, alloc_prb, dmrs_symbols, 7, 1, 0, dmrs_type, scaling)
    mask = pilot_ls_mask(alloc_prb, dmrs_symbols, dmrs_type)
    assert np.allclose(ls[mask], channel[mask], atol=1e-6), "de-boosted LS is not the channel"

    # Scatter and gather are inverses in the chain's bit order, on the data REs only.
    qm = BITS_PER_SYMBOL[modulation]
    nof_bits = int(data_re_mask(alloc_prb, dmrs_symbols).sum()) * qm
    bits = rng.integers(0, 2, size=nof_bits).astype(np.uint8)
    label = label_plane(bits, alloc_prb, dmrs_symbols, modulation)
    assert label.shape == (NOF_SUBCARRIERS, llr_channels(modulation)), label.shape
    assert llr_channels(modulation) == 112, "256QAM must use 14 * 8 channels"

    # The plane holds 1 - b, so gathering it returns the labels and the two operations are exact
    # inverses. This is the assertion that pins the label convention down.
    labels = llr_gather(label, alloc_prb, dmrs_symbols, modulation)
    assert np.array_equal(labels, 1.0 - bits), "the label plane does not hold 1 - b"

    # The LLR a perfect model emits carries the chain's sign: positive for bit 0. Under the
    # contract the logit IS the LLR, so "perfect" means z = (2 * label - 1) * large, which equals
    # (1 - 2 * b) * large. Writing it the other way inverts every LLR and still trains, so the
    # pair of assertions below is the sign check the plan requires.
    perfect_llr = (2.0 * label - 1.0) * 20.0
    quantized = llr_from_logits(np.tanh(perfect_llr / 20.0), modulation)
    gathered = llr_gather(quantized.astype(np.float32), alloc_prb, dmrs_symbols, modulation)
    assert gathered.size == nof_bits, (gathered.size, nof_bits)
    assert np.all(np.sign(gathered) == np.sign(1.0 - 2.0 * bits.astype(np.float32))), (
        "the LLR sign convention does not match the chain: positive must mean bit 0")

    # Quantization contract: mid-tread, clipped to +-LLR_MAX, never -128 and never +-LLR_INFTY.
    for name in BITS_PER_SYMBOL:
        values = llr_from_logits(np.linspace(-1.0, 1.0, 2001), name)
        assert values.min() >= -LLR_MAX and values.max() <= LLR_MAX, name
        assert values.min() != -128 and np.abs(values).max() != LLR_INFTY, name
    big = llr_from_logits(np.array([1.0, -1.0]), "64QAM")
    assert big.tolist() == [LLR_MAX, -LLR_MAX], big

    # A zero logit plane is one half, which the loss unit check relies on.
    zeros = np.zeros((NOF_SUBCARRIERS, llr_channels(modulation)), dtype=np.float32)
    assert llr_gather(zeros, alloc_prb, dmrs_symbols, modulation).size == nof_bits

    print(f"depth3 selfcheck: plane {plane.shape}, r={r:.4f}, {nof_bits} coded bits, "
          f"{llr_channels(modulation)} LLR channels, all fields round trip")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selfcheck", action="store_true",
                        help="check this implementation against the section 0.2 table")
    args = parser.parse_args()
    if args.selfcheck:
        return selfcheck()
    parser.error("nothing to do: pass --selfcheck")


if __name__ == "__main__":
    raise SystemExit(main())
