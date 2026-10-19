#!/usr/bin/env python3
"""Build a depth-3 dataset: the contract's input plane towards LLR labels that are true.

Two sources, and the difference between them matters
---------------------------------------------------
* `real`  - a capture whose transport block decoded with CRC OK. The grid is the captured one,
  impairments and all, and the labels are exact: re-encoding a CRC-OK transport block reproduces
  the transmitted bits, because the whole chain from bits to symbols is deterministic given the
  configuration the sidecar records. Nothing is synthesized, so nothing is assumed about the
  channel or the noise.
* `synthetic` - a channel fitted to a capture's own pilots, a grid built from it, and noise
  injected at a chosen SNR. The channel and the noise variance are then known exactly, which is
  what the two auxiliary heads need, and the SNR range can go above what the captures reach
  (`model_network_design.md` section 0.8bis records that nothing above 25 dB was captured).

The auxiliary targets are derived, not stored
---------------------------------------------
The noise head's target is a function of the channel and the noise variance
(`depth3_loss.noise_shape_target`), so the file carries the channel and the variance and the
trainer derives the target. Storing the derived plane would put a second copy of that formula in
the pipeline, and the two would drift.

What the file holds
-------------------
    X            [N, 612, 64] float16   the contract's input plane, absolute window
    H            [N, 612, 14, 2] float16 the true channel divided by r, i.e. the head-3 target
    r            [N] float32            reference amplitude the plane was divided by
    sigma2_over_r2 [N] float32          noise variance in units of r^2
    bits         [N, max_bytes] uint8   packed scrambled coded bits, chain order
    nof_bits     [N] int32
    tb_bits      [N, max_bytes] uint8   packed transport block
    nof_tb_bits  [N] int32
    rnti, n_id, rv [N]                  scrambling and redundancy version of the transmission
    alloc_first_prb, alloc_nof_prb [N] uint8
    dmrs_bitmap  [N] uint8              DM-RS symbol positions, one bit per symbol
    qm           [N] uint8              bits per resource element of this sample
    snr_nominal  [N] float32            the SNR the sample was built at
    source       [N] uint8              0 = real capture, 1 = synthetic
    unit                        list of capture names, for traceability

Usage
-----
    build_depth3_dataset.py --units <index.csv> --tb <dir> --corpus <dir> --out <dir> \
        --name <name> --source real --modulation 256QAM [--snrs 12,18,24]
    build_depth3_dataset.py --selfcheck --out <dir>/<name>.npz
"""

import argparse
import glob
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import depth3 as D3              # noqa: E402
import dmrs as DV                # noqa: E402
import synth as SN               # noqa: E402

sys.path.insert(0, HERE)
import dd_label as DL            # noqa: E402  (the modulation primitive `synth` itself uses)


def find_repo(start):
    """Locate the repository root by the presence of `build/` and `lib/`, not by counting dots."""
    path = start
    for _ in range(12):
        if os.path.isdir(os.path.join(path, "build")) and os.path.isdir(os.path.join(path, "lib")):
            return path
        parent = os.path.dirname(path)
        if parent == path:
            break
        path = parent
    raise SystemExit(f"cannot locate the repository root from {start}")


REPO = find_repo(HERE)

NOF_SYMBOLS = D3.NOF_SYMBOLS
BUCKET_SUBCARRIERS = D3.NOF_SUBCARRIERS
SLOTS_PER_FRAME_30KHZ = 20
MODULATION_BITS = D3.BITS_PER_SYMBOL
TDL_TAPS = 16
FFT_LEN = 1024


def read_config(prefix):
    """Read a capture's `key=value` metadata sidecar."""
    return SN.read_config(prefix)


def fit_tdl(observations, subcarriers, nof_taps=TDL_TAPS, fft_len=FFT_LEN):
    """Least-squares tapped-delay-line fit of a channel to observations at given subcarriers.

    The fit projects the observations onto the subspace a physical channel occupies, which is what
    makes a target independent of the estimator being measured. 16 taps reproduce the pilot LS of
    a capture to -28.7 dB while 8 taps only reach -2.6 dB (measured in
    `build_synth_dataset.fit_tdl_coefficients`), so the channel here genuinely needs about 16.
    """
    freq = np.asarray(subcarriers) / fft_len
    basis = np.exp(-2j * np.pi * np.outer(freq, np.arange(nof_taps)))
    coefficients, _, _, _ = np.linalg.lstsq(basis, observations, rcond=None)
    return coefficients


def channel_from_tdl(coefficients, subcarriers, fft_len=FFT_LEN):
    """Evaluate a fitted tapped-delay-line channel at the given subcarriers."""
    freq = np.asarray(subcarriers) / fft_len
    basis = np.exp(-2j * np.pi * np.outer(freq, np.arange(len(coefficients))))
    return basis @ coefficients


def allocation_subcarriers(alloc_prb):
    """Absolute subcarrier indices of a contiguous allocation, ascending."""
    return np.concatenate([np.arange(r * 12, (r + 1) * 12) for r in sorted(int(p) for p in alloc_prb)])


def data_aided_channel(grid, data_symbols, data_subcarriers, symbols, alloc_subcarriers):
    """Fit a channel per OFDM symbol to `y/x` at the data REs, where `x` is known exactly.

    This is the target the channel-estimate head is trained against on real captures. The
    transmitted symbols are re-encoded from a CRC-OK transport block rather than decided, so `y/x`
    observes the channel at every data RE, and the physical fit then removes most of the noise.
    The fit is per symbol so that a channel drifting within the slot survives, which is the same
    choice the synthetic route makes.

    The noise variance is the residual of that fit. The fit absorbs a fraction `taps / nof_re`
    (about 3 percent here) of the noise, so the value is a slight underestimate, which is stated
    rather than corrected.

    Returns `(channel, sigma2)` with the channel over the allocation subcarriers.
    """
    channel = np.zeros((NOF_SYMBOLS, len(alloc_subcarriers)), dtype=np.complex64)
    residuals = []
    for symbol in range(NOF_SYMBOLS):
        selected = data_symbols == symbol
        if not selected.any():
            continue
        subcarriers = data_subcarriers[selected]
        observations = grid[symbol, subcarriers] / symbols[selected]
        coefficients = fit_tdl(observations, subcarriers)
        channel[symbol] = channel_from_tdl(coefficients, alloc_subcarriers)
        residuals.append(observations - channel_from_tdl(coefficients, subcarriers))
    sigma2 = float(np.mean(np.abs(np.concatenate(residuals)) ** 2))

    # The DM-RS symbols carry no data, so the fit above leaves them at zero -- and the DM-RS symbols are
    # exactly where `pilot_ls_consistency` looks, because that is where the pilot LS is defined. Comparing
    # the pilot LS against a zero channel does not test the DM-RS convention; it tests whether the pilot
    # power happens to equal the noise, so it rejected every capture with a decent pilot SNR and accepted
    # the ones near 0 dB. Measured 2026-10-16 on the s4_d corpus: 11 of 60 captures passed, all of them the
    # low-SNR ones. The channel drifts slowly inside a slot, so interpolating it between the neighbouring
    # DATA symbols gives the comparison something real to compare against.
    filled = [s for s in range(NOF_SYMBOLS) if np.any(channel[s] != 0.0)]
    for symbol in range(NOF_SYMBOLS):
        if np.any(channel[symbol] != 0.0) or not filled:
            continue
        below = [s for s in filled if s < symbol]
        above = [s for s in filled if s > symbol]
        if below and above:
            low, high = below[-1], above[0]
            weight = (symbol - low) / float(high - low)
            channel[symbol] = (1.0 - weight) * channel[low] + weight * channel[high]
        elif below:
            channel[symbol] = channel[below[-1]]
        else:
            channel[symbol] = channel[above[0]]
    return channel, sigma2


def synthetic_channel(prefix, cfg, alloc, sub, grid):
    """Fit a channel to the capture's DM-RS pilots, per DM-RS symbol, then interpolate in time.

    The same construction `build_synth_dataset.truth_from_pilots` uses: a per-symbol fit so a
    channel that drifts within the slot survives, held flat outside the pilot symbols.
    """
    dmrs_symbols = [int(x) for x in cfg["dmrs_symbols"].split(",")]
    pilots = DV.pilot_subcarriers(sorted(alloc), dmrs_type=int(cfg["dmrs_type"]))
    slot = int(cfg["slot"]) % SLOTS_PER_FRAME_30KHZ
    fits = {}
    for symbol in dmrs_symbols:
        sequence = DV.sequence(slot, symbol, int(cfg["dmrs_scrambling_id"]), int(cfg["dmrs_n_scid"]), alloc)
        fits[symbol] = fit_tdl(grid[symbol, pilots] / sequence, pilots)

    clean = np.zeros((NOF_SYMBOLS, len(sub)), dtype=np.complex64)
    low, high = min(dmrs_symbols), max(dmrs_symbols)
    for symbol in range(NOF_SYMBOLS):
        if symbol in fits:
            clean[symbol] = channel_from_tdl(fits[symbol], sub)
        elif symbol < low:
            clean[symbol] = channel_from_tdl(fits[low], sub)
        elif symbol > high:
            clean[symbol] = channel_from_tdl(fits[high], sub)
        else:
            below = max(s for s in dmrs_symbols if s < symbol)
            above = min(s for s in dmrs_symbols if s > symbol)
            weight = (symbol - below) / (above - below)
            clean[symbol] = ((1.0 - weight) * channel_from_tdl(fits[below], sub)
                             + weight * channel_from_tdl(fits[above], sub))
    return clean


def candidate_units(index_path, tb_dir, corpus_dir, modulation, min_prb, min_slot,
                    require_off_prb0, max_prb=0, need_tb=True):
    """Select units that can be re-encoded.

    `build_synth_dataset` also rejected allocations starting at PRB 0 and early slots, because
    those two coincidences each hid a convention bug once (the DM-RS PRG start and the slot number
    the sequence is seeded from). Both conventions are now settled against the chain's own dump
    (`dmrs.py`), and `pilot_ls_consistency` checks them again per sample against the capture's own
    data REs, which is a stronger guarantee than refusing the cases that could hide a bug: the
    guard only ensured a bug could not hide, the check ensures the convention is right. The old
    restriction is still available behind `require_off_prb0`, and it costs five sixths of the
    CRC-OK units (243 of 1153 start off PRB 0).
    """
    units = []
    with open(index_path) as handle:
        next(handle)
        for line in handle:
            fields = line.strip().split(",")
            if len(fields) < 6:
                continue
            # The CRC column decides whether a REAL sample can be built (the payload is the dump).
            # A synthetic sample brings its own payload, so a capture the reference chain could not
            # decode is still a perfectly good CHANNEL -- and those are exactly the captures a
            # failing cell is made of.
            if need_tb and fields[2] != "OK":
                continue
            name = fields[0]
            tb_path = os.path.join(tb_dir, name + ".bits")
            prefix = os.path.join(corpus_dir, name)
            if not (os.path.exists(prefix + ".bin") and os.path.exists(prefix + "_h.bin")):
                continue
            # A synthetic sample's payload is OUR choice, sized from the target code rate, so the
            # dumped transport block is not needed at all -- and requiring it silently threw away
            # every capture the reference chain could not decode, which is exactly the material
            # that teaches a cell the reference chain fails in (design section 19.209(e)).
            if need_tb and not os.path.exists(tb_path):
                continue
            cfg = read_config(prefix)
            if cfg.get("modulation") != modulation:
                continue
            alloc = [int(x) for x in cfg["alloc_prb"].split(",")]
            if alloc != list(range(alloc[0], alloc[0] + len(alloc))):
                continue
            if int(cfg["slot"]) < min_slot or len(alloc) < min_prb:
                continue
            if max_prb and len(alloc) > max_prb:
                continue
            if require_off_prb0 and min(alloc) == 0:
                continue
            units.append((name, prefix, cfg, alloc, tb_path))
    return units


def pilot_ls_consistency(ls, channel, alloc_prb, dmrs_symbols, dmrs_type, sigma2, scaling,
                         window_subcarriers=None):
    """How well the pilot LS agrees with the data-aided channel, as a scale-free correlation.

    The pilot LS is built from the DM-RS sequence this module generates, while the channel comes
    from the DATA REs, so the two are independent. Their agreement is therefore a test of the
    DM-RS conventions against the capture itself: a wrong PRG start, a wrong slot reduction or a
    wrong sign would make the correlation collapse, and none of those is visible in the shapes.

    This was a residual-power ratio until 2026-10-16, and for real captures it was measuring
    nothing of the kind. `data_aided_channel` fits the channel on the DATA symbols only, so it
    holds zero on the DM-RS symbols -- which is exactly where the pilot mask looks. The "residual"
    was therefore the pilot power itself and the ratio was the pilot SNR in units of the noise, so
    the check rejected every capture with a decent pilot SNR and accepted the ones near 0 dB:
    measured on the s4_d corpus, 11 of 60 passed and every one of them was a low-SNR capture. The
    fit now interpolates across the DM-RS symbols (see `data_aided_channel`), which makes the
    comparison defined, and the correlation is its scale-free form -- it does not depend on the
    noise model that the old ratio assumed, which was itself approximate because the data fit's
    residual carries model error as well as noise. The caller's window, 0.5 to 2.0, applies to the
    correlation unchanged: below 0.5 the two estimates are not describing the same channel.

    `window_subcarriers` slices the LS and the mask down to the allocation, which is where the
    caller's channel is defined; the channel is already restricted, so it is not sliced again.
    """
    mask = D3.pilot_ls_mask(alloc_prb, dmrs_symbols, dmrs_type)
    ls = np.asarray(ls)
    channel = np.asarray(channel)
    if window_subcarriers is not None:
        ls = ls[:, window_subcarriers]
        mask = mask[:, window_subcarriers]
    assert ls.shape == channel.shape, (ls.shape, channel.shape)
    if mask.sum() == 0:
        return float("nan")
    pilot = ls[mask]
    data = channel[mask]
    denominator = float(np.sqrt(np.mean(np.abs(pilot) ** 2) * np.mean(np.abs(data) ** 2)))
    if denominator == 0.0:
        return float("nan")
    return float(abs(np.mean(pilot * np.conj(data))) / denominator)


def load_transport_block(tb_path, source):
    """Read a dumped transport block, and refuse the ones that carry no payload.

    The decoder's dump writes its `transport_block` buffer, and for most of the corpus that buffer
    is ALL ZEROS while the chain still reports `crc=OK`. The reason is that an all-zero word is a
    valid LDPC codeword (the syndrome of zero is zero) and its CRC-24A is also zero, so a decode
    that returns zeros passes both checks. Measured on the 2026-10-12 dump: of 3901 index rows,
    2757 have no file, 1068 are all zeros and 62 carry a payload.

    For the `real` source the payload IS the label, so a zero one is refused rather than used.
    For the `synthetic` source the payload is our choice, so only the LENGTH is taken from the
    dump, which keeps the LDPC code rate of the pair realistic.
    """
    bits = np.unpackbits(np.fromfile(tb_path, dtype=np.uint8))
    if bits.size == 0:
        raise ValueError("the transport block file is empty")
    if source == "real" and not bits.any():
        raise ValueError("the dumped transport block is all zeros: the decode produced no payload")
    return bits


def build_sample(name, prefix, cfg, alloc, tb_path, source, snr_db, seed, payload="capture",
                 synth_rate=None, target_dmrs=None):
    """Build one dataset sample, or raise ValueError with the reason it cannot be built.

    `target_dmrs` (synthetic only) puts the DM-RS at a chosen set of symbols instead of the capture's.
    The CHANNEL is still fitted from the capture's own pilots -- `synthetic_channel` reads those from
    `cfg`, which is why the two lists have to be separate: the pilots exist where the capture put
    them, while the sample we build may place them elsewhere. This is what lets a pattern the corpus
    has no transmission of (256QAM at 2,7,11, whose captures all used 2,11) be manufactured from the
    captures that do have the channel, instead of needing a new radio session first.
    """
    modulation = cfg["modulation"]
    qm = MODULATION_BITS[modulation]
    dmrs_symbols = list(target_dmrs) if target_dmrs else [int(x) for x in cfg["dmrs_symbols"].split(",")]
    dmrs_mask = sum(1 << s for s in dmrs_symbols)
    bwp = int(cfg["bwp_size_rb"])
    grid_file = np.fromfile(prefix + ".bin", dtype=np.complex64)
    grid_file = grid_file[:NOF_SYMBOLS * bwp * 12].reshape(NOF_SYMBOLS, bwp * 12)

    # The redundancy version of a SYNTHETIC sample is ours to choose, and it has to be 0. The
    # captures are mostly rv=2, which selects from the circular buffer with a start offset that
    # carries almost no systematic bits: measured on a 46 PRB unit, rv=2 transmits 0.0 percent
    # systematic, rv=1 7.8 percent, against rv=0 79 percent. An rv=2 transmission is a HARQ
    # RETRANSMISSION and is only decodable once combined with an earlier one in the soft buffer,
    # which a single-slot replay does not have. Training targets are unaffected either way, but a
    # coded-BLER criterion is meaningless on a block that cannot be decoded alone.
    rv = 0 if source == "synthetic" else int(cfg["rv"])

    if synth_rate is not None:
        # THE POINT OF THIS MODE (design section 19.209(e)): the payload size -- hence the code
        # rate -- is chosen by us, so the label does not depend on the reference chain having
        # decoded this cell. The number of coded bits a window carries does not depend on the
        # payload, so one probe with a tiny block measures it.
        _, _, probe = SN.encode_symbols(np.zeros(8, dtype=np.uint8), len(alloc), NOF_SYMBOLS, qm,
                                        dmrs_mask, int(cfg["n_id"]), int(cfg["rnti"]), rv,
                                        return_bits=True)
        nof_tb_bits = max(8, int(round(float(synth_rate) * probe.size)) // 8 * 8)
        tb_bits = np.random.default_rng(seed).integers(0, 2, size=nof_tb_bits).astype(np.uint8)
    else:
        tb_bits = load_transport_block(tb_path, source)
    if payload == "random" and synth_rate is None:
        # The synthetic route chooses its own payload, so the dump only supplies the size. The
        # bits are then exact by construction, which is the whole point of building the pair.
        tb_bits = np.random.default_rng(seed).integers(0, 2, size=tb_bits.size).astype(np.uint8)
    symbols, data_pos, bits = SN.encode_symbols(tb_bits, len(alloc), NOF_SYMBOLS, qm, dmrs_mask,
                                                int(cfg["n_id"]), int(cfg["rnti"]), rv,
                                                return_bits=True)

    sub = allocation_subcarriers(alloc)
    # The transmit positions in absolute subcarriers: `data_pos` is allocation relative and the
    # allocation is contiguous, which `candidate_units` enforces.
    offset = min(alloc) * 12
    data_symbols = np.array([p[0] for p in data_pos], dtype=np.int32)
    data_subcarriers = np.array([p[1] + offset for p in data_pos], dtype=np.int32)

    if source == "real":
        grid = grid_file
        channel_alloc, sigma2 = data_aided_channel(grid, data_symbols, data_subcarriers,
                                                   symbols, sub)
        snr_effective = float(10.0 * np.log10(np.mean(np.abs(channel_alloc) ** 2) / sigma2))
        if snr_db is not None:
            # Augmentation by noise resampling (`design` section 3.6): the bits are unchanged, so
            # the labels stay exact, and only the noise variance grows. Reaching a HIGHER SNR than
            # the capture has is impossible this way, so those points are skipped.
            if snr_db > snr_effective:
                raise ValueError(f"target {snr_db} dB is above the capture's {snr_effective:.1f} dB")
            signal_power = float(np.mean(np.abs(grid[data_symbols, data_subcarriers]) ** 2))
            sigma2_target = signal_power / (10.0 ** (snr_db / 10.0))
            extra = sigma2_target - sigma2
            if extra <= 0.0:
                raise ValueError("no extra noise needed at this SNR")
            rng = np.random.default_rng(seed)
            noise = rng.standard_normal(grid.shape) + 1j * rng.standard_normal(grid.shape)
            grid = grid + noise * np.sqrt(extra / 2.0)
            sigma2 = sigma2_target
    else:
        # Synthetic: a physical fit to the capture's pilots is the truth, and the grid is built
        # from it, so the channel is known exactly rather than estimated.
        channel = synthetic_channel(prefix, cfg, alloc, sub, grid_file)
        if snr_db is None:
            raise ValueError("the synthetic source needs a target SNR")
        full = np.zeros_like(grid_file)
        full[:, sub] = channel
        # `build_clean` writes the data REs and the pilots of the allocation; the channel covers
        # the allocation, so a position outside it stays zero, which is what an unallocated RE is.
        grid = SN.build_clean(cfg, full, symbols, data_pos, alloc, dmrs_symbols)
        signal_power = float(np.mean(np.abs(grid[np.abs(grid) > 0]) ** 2))
        sigma2 = signal_power / (10.0 ** (snr_db / 10.0))
        rng = np.random.default_rng(seed)
        noise = rng.standard_normal(grid.shape) + 1j * rng.standard_normal(grid.shape)
        grid = grid + noise * np.sqrt(sigma2 / 2.0)
        channel_alloc = channel

    meta = {
        "nslot": int(cfg["slot"]) % SLOTS_PER_FRAME_30KHZ,
        "n_id": int(cfg["dmrs_scrambling_id"]),
        "n_scid": int(cfg["dmrs_n_scid"]),
        "dmrs_type": int(cfg["dmrs_type"]),
        "nof_cdm_groups_without_data": int(cfg["dmrs_nof_cdm_groups_without_data"]),
        "nof_tx_layers": int(cfg["nof_tx_layers"]),
        "nof_rx_ports": 1,
        "active_dmrs_ports": 1,
        "nof_symbols": int(cfg["nof_symbols"]),
        "start_symbol_index": int(cfg["start_symbol_index"]),
        "alloc_nof_prb": len(alloc),
        "dc_position": int(cfg["dc_position"]) if cfg.get("dc_position") else None,
    }
    plane, r = D3.pack_input(grid, alloc, dmrs_symbols, meta)

    # The pilots in the plane are built from the DM-RS sequence this module generates; the channel
    # comes from the data REs. Checking one against the other validates the DM-RS conventions
    # against the capture, per sample, which is what lets an allocation at PRB 0 be used at all.
    ls = D3.pilot_ls(grid, alloc, dmrs_symbols, meta["nslot"], meta["n_id"], meta["n_scid"],
                     meta["dmrs_type"], D3.dmrs_amplitude_scaling(meta["nof_cdm_groups_without_data"]))
    ratio = pilot_ls_consistency(ls, channel_alloc, alloc, dmrs_symbols, meta["dmrs_type"],
                                 sigma2, D3.dmrs_amplitude_scaling(meta["nof_cdm_groups_without_data"]),
                                 window_subcarriers=sub)
    if not (0.5 < ratio < 2.0):
        raise ValueError(f"the pilot LS does not match the data-aided channel (ratio {ratio:.3f}); "
                         f"the DM-RS convention is wrong for this capture")

    # The target channel covers the allocation only; everywhere else the network's estimate is
    # unconstrained, so those entries are masked out of the loss rather than trained towards zero.
    # It is stored DIVIDED BY r, because that is what the head produces: the contract's head 3
    # emits the channel at the reference amplitude and the seam multiplies r back in. Storing the
    # physical channel instead would train the head against a target r times too large, which at
    # r = 0.76 looks like a plain loss of accuracy rather than a units error.
    channel_grid = np.zeros_like(grid_file)
    channel_grid[:, sub] = channel_alloc
    channel_norm = np.zeros((BUCKET_SUBCARRIERS, NOF_SYMBOLS, 2), dtype=np.float16)
    channel_norm[sub, :, 0] = (channel_grid[:, sub] / r).T.real
    channel_norm[sub, :, 1] = (channel_grid[:, sub] / r).T.imag

    sample = {
        "X": plane.astype(np.float16),
        "H": channel_norm,
        "r": np.float32(r),
        # The transport block itself, not only the coded bits: the codeblock layout
        # (segmentation, base graph, lifting size, per-block rate-matched length) follows from it,
        # so a coded-BLER evaluation can be reproduced from the file alone. Without it the
        # synthetic payload is unrecoverable, which is exactly the kind of hole that makes a
        # dataset unusable six months later.
        "tb_bits": np.packbits(tb_bits.astype(np.uint8)),
        "nof_tb_bits": np.int32(tb_bits.size),
        "rnti": np.uint16(int(cfg["rnti"])),
        "n_id": np.uint16(int(cfg["n_id"])),
        "rv": np.uint8(rv),
        "sigma2_over_r2": np.float32(sigma2 / (r * r)),
        "pilot_ls_ratio": np.float32(ratio),
        "bits": np.packbits(bits),
        "nof_bits": np.int32(bits.size),
        "alloc_first_prb": np.uint8(min(alloc)),
        "alloc_nof_prb": np.uint8(len(alloc)),
        # One bit per OFDM symbol, so 14 bits: symbol 11 alone needs more than a byte.
        "dmrs_bitmap": np.uint16(dmrs_mask),
        "qm": np.uint8(qm),
        "snr_nominal": np.float32(snr_db if snr_db is not None else np.nan),
    }
    return sample, bits, grid, channel_grid, sigma2, r


def assemble(samples, keys_arrays):
    """Stack per-sample dicts into arrays, padding the packed bits to the longest sample."""
    out = {}
    out["X"] = np.stack([s["X"] for s in samples])
    out["H"] = np.stack([s["H"] for s in samples])
    out["r"] = np.array([s["r"] for s in samples], dtype=np.float32)
    out["sigma2_over_r2"] = np.array([s["sigma2_over_r2"] for s in samples], dtype=np.float32)
    out["pilot_ls_ratio"] = np.array([s["pilot_ls_ratio"] for s in samples], dtype=np.float32)
    max_bytes = max(len(s["bits"]) for s in samples)
    bits = np.zeros((len(samples), max_bytes), dtype=np.uint8)
    for index, sample in enumerate(samples):
        bits[index, :len(sample["bits"])] = sample["bits"]
    out["bits"] = bits
    max_tb_bytes = max(len(s["tb_bits"]) for s in samples)
    transport = np.zeros((len(samples), max_tb_bytes), dtype=np.uint8)
    for index, sample in enumerate(samples):
        transport[index, :len(sample["tb_bits"])] = sample["tb_bits"]
    out["tb_bits"] = transport
    for name in ("nof_bits", "nof_tb_bits", "alloc_first_prb", "alloc_nof_prb", "dmrs_bitmap",
                 "qm", "snr_nominal", "rnti", "n_id", "rv"):
        out[name] = np.array([s[name] for s in samples])
    out["source"] = np.array([s["source"] for s in samples], dtype=np.uint8)
    out["unit"] = np.array(keys_arrays, dtype=object)
    return out


def selfcheck(path):
    """Check that a stored dataset is internally consistent and that its labels are true.

    The strongest check in here rebuilds the transmitted symbols from the stored bits and compares
    them with the stored grid through the stored channel: the residual must be the stored noise
    variance. A dataset that fails it has a wrong label order, a wrong channel, a wrong window or
    a wrong noise level, and none of those is visible from the file's shapes.
    """
    data = np.load(path, allow_pickle=True)
    n = len(data["X"])
    print(f"{path}: {n} samples, X{data['X'].shape} H{data['H'].shape}")
    # Spread the checks over the whole file rather than taking its head: the samples are ordered
    # by unit and then by SNR, so the first 25 would all come from one capture at low SNR.
    checked = np.unique(np.linspace(0, n - 1, min(n, 60)).astype(int))
    ratios = []
    for index in checked:
        plane = data["X"][index].astype(np.float32)
        assert plane.shape == (BUCKET_SUBCARRIERS, D3.NOF_CHANNELS), plane.shape
        first_prb = int(data["alloc_first_prb"][index])
        nof_prb = int(data["alloc_nof_prb"][index])
        alloc = list(range(first_prb, first_prb + nof_prb))
        dmrs_bitmap = int(data["dmrs_bitmap"][index])
        dmrs_symbols = [s for s in range(NOF_SYMBOLS) if (dmrs_bitmap >> s) & 1]
        qm = int(data["qm"][index])
        modulation = {v: k for k, v in MODULATION_BITS.items()}[qm]
        sub = allocation_subcarriers(alloc)

        # The plane must be readable as the contract says, including the reserved channel.
        back = D3.unpack_input(plane, alloc, dmrs_symbols, 1)
        assert back["reserved"].sum() == 0.0, "the reserved channel is not zero"
        assert np.array_equal(back["allocation_mask"].astype(bool),
                              D3.allocated_subcarriers(alloc)), "allocation mask"
        # The plane is fp16 by contract, so a normalized metadata scalar carries about three
        # decimal digits and an exact comparison would fail on the storage, not on the value.
        assert abs(float(back["metadata"]["alloc_nof_prb_over_51"]) - nof_prb / 51) < 1e-3, \
            "metadata alloc width"
        assert abs(float(back["metadata"]["nof_dmrs_symbols"]) - len(dmrs_symbols)) < 1e-3, \
            "metadata DM-RS symbol count"

        # Rebuild the transmitted symbols from the stored bits and compare the grid against them
        # through the stored channel. The residual power must be the stored noise variance.
        nof_bits = int(data["nof_bits"][index])
        bits = np.unpackbits(data["bits"][index])[:nof_bits]
        symbols = np.asarray(DL.modulate(bits.tolist(), qm), dtype=np.complex64)

        scale = float(data["r"][index])
        grid = back["grid"] * scale
        channel = (data["H"][index][:, :, 0].astype(np.float32)
                   + 1j * data["H"][index][:, :, 1].astype(np.float32)).T * scale
        sigma2 = float(data["sigma2_over_r2"][index]) * scale * scale

        # Walk the data REs exactly as the contract's gather does, so a wrong bit order shows up
        # as a residual that is nothing like the noise variance.
        predicted = np.zeros_like(grid)
        cursor = 0
        for symbol in range(NOF_SYMBOLS):
            if symbol in dmrs_symbols:
                continue
            for subcarrier in np.flatnonzero(D3.allocated_subcarriers(alloc)):
                predicted[symbol, subcarrier] = channel[symbol, subcarrier] * symbols[cursor]
                cursor += 1
        assert cursor == len(symbols), (cursor, len(symbols))

        data_mask = D3.data_re_mask(alloc, dmrs_symbols).T
        residual = np.abs(grid[data_mask] - predicted[data_mask]) ** 2
        ratio = float(residual.mean() / sigma2)
        ratios.append(ratio)
        assert 0.9 < ratio < 1.1, (
            f"sample {index} ({data['unit'][index]}): the grid is not its channel times its "
            f"symbols plus the stored noise (residual/sigma2 = {ratio:.3f})")
        # The constellation has unit average power, but the empirical mean of a finite sequence is
        # not exactly one: 256QAM has a fourth moment of 1.40, so 7000 symbols land within about
        # 1 percent of it. A tolerance tighter than that fails on the sample, not on the data.
        power = float(np.mean(np.abs(symbols) ** 2))
        assert abs(power - 1.0) < 0.05, f"constellation power {power:.4f} is not unit"

        # The pilot LS inside the plane must agree with the channel, which came from the data REs.
        # A wrong DM-RS convention shows up here and nowhere else.
        ls = back["pilot_ls"] * scale
        ratio_ls = pilot_ls_consistency(ls, channel[:, sub], alloc, dmrs_symbols,
                                        int(back["metadata"]["dmrs_type"]), sigma2,
                                        float(back["metadata"]["scaling"]),
                                        window_subcarriers=sub)
        assert 0.5 < ratio_ls < 2.0, (
            f"sample {index} ({data['unit'][index]}): the pilot LS is not the channel "
            f"(ratio {ratio_ls:.3f})")

        # The payload must be recoverable, because the codeblock layout follows from its length.
        nof_tb = int(data["nof_tb_bits"][index])
        tb_bits = np.unpackbits(data["tb_bits"][index])[:nof_tb]
        assert tb_bits.size == nof_tb, "the stored transport block is shorter than it claims"

        # The label plane, the bits and the gather must agree in the chain's order.
        label = D3.label_plane(bits, alloc, dmrs_symbols, modulation)
        assert np.array_equal(D3.llr_gather(label, alloc, dmrs_symbols, modulation), 1.0 - bits), \
            "the stored bits are not in the chain's order"

    print(f"  residual/sigma2 over {len(ratios)} checked samples: "
          f"min {min(ratios):.3f} median {np.median(ratios):.3f} max {max(ratios):.3f}")
    print(f"  pilot-LS/channel ratio: median {np.median(data['pilot_ls_ratio'][checked]):.3f} "
          f"(1.0 means the DM-RS sequence matches the capture)")
    print("  every checked sample: plane, masks, metadata, DM-RS, label order and noise agree")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--units")
    parser.add_argument("--tb")
    parser.add_argument("--corpus")
    parser.add_argument("--out", required=True)
    parser.add_argument("--name", default="depth3")
    parser.add_argument("--source", choices=("real", "synthetic"), default="real",
                        help="'real' uses the captured grid and the captured payload, so the "
                             "labels are the transmission itself; it is blocked while the TB dump "
                             "returns zeros. 'synthetic' builds the grid from a channel fitted to "
                             "the capture's pilots and picks its own payload, so the labels are "
                             "exact by construction and the SNR is free.")
    parser.add_argument("--modulation", default="256QAM", choices=sorted(MODULATION_BITS))
    parser.add_argument("--snrs", default="",
                        help="comma separated target SNRs. For 'real' these are augmentation "
                             "points reached by adding noise, so they must be BELOW the capture's "
                             "own SNR; for 'synthetic' they are the SNR the grid is built at.")
    parser.add_argument("--include-natural", action="store_true",
                        help="also keep each real capture at its own SNR")
    parser.add_argument("--min-prb", type=int, default=43)
    parser.add_argument("--max-prb", type=int, default=0,
                        help="widest allocation to keep, 0 for no limit. The trained envelope is "
                             "a RANGE, and this is what isolates one end of it: a narrow-only set "
                             "is how the model's behaviour on allocations it never saw gets a "
                             "number instead of a suspicion")
    parser.add_argument("--min-slot", type=int, default=0,
                        help="reject slots below this. The old pipeline used 40 to keep the "
                             "slot-number convention from hiding; the per-sample pilot check "
                             "replaces that guard.")
    parser.add_argument("--require-off-prb0", action="store_true",
                        help="reject allocations starting at PRB 0. Costs five sixths of the "
                             "CRC-OK units; kept so the old selection can be reproduced.")
    parser.add_argument("--synth-prb", type=int, default=0,
                        help="SYNTHETIC only: build every sample on a window THIS many PRBs wide, "
                             "instead of the capture's own allocation. The window is taken inside "
                             "the capture's allocation, so the channel it is fitted from is really "
                             "there. This is how a cell the corpus has none of -- a narrow, "
                             "high-rate grant -- is manufactured from captures that have the "
                             "channel but not the cell")
    parser.add_argument("--synth-start-prb", type=int, default=-1,
                        help="SYNTHETIC only: first PRB of the window (default: the capture's own "
                             "first PRB, clamped so the window fits)")
    parser.add_argument("--synth-rate", type=float, default=0.0,
                        help="SYNTHETIC only: target code rate (0 = take the dumped block's size). "
                             "The payload is then sized from the window's own coded-bit count, so "
                             "the label is exact by construction and no transport-block dump, and "
                             "no decodable reference reception, is needed")
    parser.add_argument("--synth-dmrs-symbols", default="",
                        help="SYNTHETIC only: place the DM-RS at these symbol indices (e.g. \"2,7,11\") "
                             "instead of the capture's. The channel still comes from the capture's own "
                             "pilots; what changes is where the sample transmits its DM-RS. Used to "
                             "manufacture a modulation x pattern cell the corpus has no transmission of")
    parser.add_argument("--max-units", type=int, default=0)
    parser.add_argument("--seed", type=int, default=20261013)
    parser.add_argument("--selfcheck", action="store_true")
    args = parser.parse_args()

    if args.selfcheck:
        return selfcheck(args.out)
    missing = [name for name in ("units", "tb", "corpus") if getattr(args, name) is None]
    if missing:
        parser.error("missing required arguments: " + ", ".join("--" + m for m in missing))

    retarget = args.source == "synthetic" and args.synth_prb > 0
    if (args.synth_prb or args.synth_start_prb >= 0 or args.synth_rate) and args.source != "synthetic":
        parser.error("--synth-prb/--synth-start-prb/--synth-rate are SYNTHETIC only: the real "
                     "source is the capture's own transmission")
    if retarget and args.synth_rate <= 0.0:
        parser.error("--synth-prb needs --synth-rate: a window on its own does not name a cell")
    target_dmrs = None
    if args.synth_dmrs_symbols:
        if args.source != "synthetic":
            parser.error("--synth-dmrs-symbols is SYNTHETIC only")
        target_dmrs = [int(x) for x in args.synth_dmrs_symbols.split(",") if x]
        if not target_dmrs or min(target_dmrs) < 0 or max(target_dmrs) >= NOF_SYMBOLS:
            parser.error(f"--synth-dmrs-symbols must name symbols in 0..{NOF_SYMBOLS - 1}")
    units = candidate_units(args.units, args.tb, args.corpus, args.modulation,
                            1 if retarget else args.min_prb,
                            args.min_slot, args.require_off_prb0,
                            0 if retarget else args.max_prb,
                            need_tb=not retarget)
    if retarget:
        # Keep only the captures whose own allocation can host the window: outside it there are no
        # pilots to fit the channel from, so a sample built there would carry a channel nobody
        # measured -- the failure mode this whole file exists to avoid.
        kept = []
        for unit in units:
            alloc = unit[3]
            if len(alloc) < args.synth_prb:
                continue
            kept.append(unit)
        units = kept
    if args.max_units:
        units = units[:args.max_units]
    print(f"{len(units)} candidate units (CRC OK, {args.modulation}, contiguous allocation, "
          f"slot >= {args.min_slot}, width >= {args.min_prb} PRB)")

    snrs = [float(x) for x in args.snrs.split(",") if x] if args.snrs else []
    samples, names, skipped = [], [], []
    for index, (name, prefix, cfg, alloc, tb_path) in enumerate(units):
        points = list(snrs) if snrs else [None]
        if args.include_natural and snrs:
            points = [None] + points
        made = 0
        # One seed per unit, not per point: the synthetic payload then depends on the unit alone,
        # so re-encoding a large transport block is paid once instead of once per SNR.
        if retarget:
            start = alloc[0] if args.synth_start_prb < 0 else args.synth_start_prb
            start = max(alloc[0], min(start, alloc[-1] - args.synth_prb + 1))
            alloc = list(range(start, start + args.synth_prb))
        for point_index, snr in enumerate(points):
            try:
                sample, _, _, _, _, _ = build_sample(name, prefix, cfg, alloc, tb_path,
                                                     args.source, snr,
                                                     args.seed + index * 17,
                                                     payload="random" if args.source == "synthetic"
                                                     else "capture",
                                                     synth_rate=args.synth_rate if retarget else None,
                                                     target_dmrs=target_dmrs)
            except (AssertionError, ValueError, np.linalg.LinAlgError) as error:
                if snr is None or args.source == "synthetic":
                    skipped.append((name, str(error).split("\n")[0][:100]))
                    break
                continue
            sample["source"] = 0 if args.source == "real" else 1
            samples.append(sample)
            names.append(f"{name}@{snr if snr is not None else 'natural'}")
            made += 1
        if (index + 1) % 25 == 0:
            print(f"  ... {index + 1}/{len(units)} units, {len(samples)} samples")

    if skipped:
        print(f"{len(skipped)} unit(s) skipped, first few:")
        for name, reason in skipped[:8]:
            print(f"  {name}: {reason}")
    if not samples:
        raise SystemExit("no samples produced")

    os.makedirs(args.out, exist_ok=True)
    arrays = assemble(samples, names)
    path = os.path.join(args.out, args.name + ".npz")
    np.savez_compressed(path, **arrays)
    print(f"wrote {path}: {len(samples)} samples, X{arrays['X'].shape} "
          f"({os.path.getsize(path) / 1e6:.1f} MB)")
    levels = arrays["snr_nominal"]
    for level in sorted(set(np.round(levels[~np.isnan(levels)], 1))):
        count = int((np.abs(levels - level) < 0.05).sum())
        print(f"  {level:5.1f} dB : {count:5d} samples")
    print(f"  natural  : {int(np.isnan(levels).sum()):5d} samples")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
