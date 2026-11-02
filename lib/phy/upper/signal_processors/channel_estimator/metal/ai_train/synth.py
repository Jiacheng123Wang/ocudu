#!/usr/bin/env python3
"""Build a noiseless received grid from a known channel and known transmitted symbols.

This is the shared core of the synthesis route: `build_synth_dataset.py` uses it to make
supervised pairs whose target is true by construction, and `synth_noisy.py` (in the
llr_ai_detection work area) uses it interactively. It lives here, next to the other training
primitives, because everything in it had to be pinned down against the chain and a second copy
would drift.

What has to be right, and why each one was wrong at least once
-------------------------------------------------------------
* **The DM-RS sequence** (`dmrs.py`) - the pilots are the only REs whose transmitted value is
  known a priori, so the whole grid hinges on them. Three separate conventions in it were
  settled by dumping the estimator's own output (see that module).
* **The PRG slot** - the estimator seeds it from `slot_point::slot_index()`, the slot number
  within the radio frame, not the absolute slot count.
* **The layout width** - `encode_symbols` lays the data symbols out over the ALLOCATION's
  subcarrier grid, and `build_clean` uses those indices as offsets from the allocation start.
  Passing the BWP width over-counts the data REs and changes the rate-matching length.
* **The reserved REs** - with two CDM groups without data, the odd subcarrier offsets of a
  DM-RS symbol carry no data and measure ~1e-4 of the pilot power, so they stay at zero.
* **The control elements** - a reception that multiplexes control information on the shared channel
  does not carry the shared channel on every element of a data symbol, and it does not scramble it
  from the sequence's own start (`pusch_uci`). Laying the shared channel over the whole allocation
  is only right when there is no control information, which is why every unit that carried some was
  rejected or, worse, admitted with labels that correlate with nothing.

Three of those four are invisible whenever the allocation starts at PRB 0, and the slot one is
invisible below 20 slots per frame. That coincidence has now hidden a bug three times in this
pipeline, so callers are expected to use allocations that do not start at PRB 0.
"""

import numpy as np

import dmrs as DV

NOF_SYMBOLS = 14
NOF_SUBCARRIERS_PER_RB = 12
# 30 kHz SCS: 20 slots per radio frame. `slot_point::slot_index()` reduces modulo this.
SLOTS_PER_FRAME_30KHZ = 20


def encode_symbols(tb_bits, n_prb, n_syms, mod, dmrs_mask, n_id, rnti, rv, n_layers=1,
                   return_bits=False, uci=None):
    """Re-encode a transport block into the transmitted data symbols.

    Follows the same primitive chain as `build_labels.build_label` (segment -> LDPC encode ->
    rate match -> bit interleave -> scramble -> modulate) and imports those primitives from it,
    so the two cannot drift. It exists because `build_label` returns a SMOOTHED channel estimate
    rather than the transmitted symbols.

    `n_prb` is the ALLOCATION width: the returned positions are in the allocation's own
    coordinates, starting at 0. Passing the BWP width instead over-counts the data REs, which
    changes the rate-matching length; the code-block assertion below catches that.

    `return_bits` additionally returns the SCRAMBLED coded bits in the order `modulate` consumes
    them. That sequence is the depth-3 network's training target: it is known exactly, it is in
    the scrambling domain the contract's LLRs live in, and it has to be taken before modulation
    because modulation is not invertible from the symbols back to the bits. Callers that only
    want the symbols keep the two-value return.

    `uci` is the reception's control payload (`pusch_uci.UciLayout`) or None when there is none.
    It removes the elements the control parts take, shortens the codeword by the ones they REMOVE
    rather than puncture, and moves the scrambling to each element's position in the full
    allocation. `data_pos` is the elements that carry a shared-channel symbol, and the returned bits
    are the scrambled symbols' bits in that same order.

    Returns (symbols, data_pos) or (symbols, data_pos, scrambled).
    """
    import build_labels as BL
    import pusch_uci as UCI

    dmrs_syms = [s for s in range(NOF_SYMBOLS) if (dmrs_mask >> s) & 1]
    layout = UCI.data_layout(n_prb, n_syms, dmrs_syms, mod, uci)
    data_pos = layout.data_pos

    code_blocks, bg, zc, kb, e_total, lengths = codeblock_layout(tb_bits, n_prb, n_syms, mod,
                                                                dmrs_mask, uci, layout=layout)

    coded = []
    for index, e_cb in enumerate(lengths):
        if kb < len(code_blocks[index]):
            raise ValueError(f"K_b {kb} < code block {len(code_blocks[index])}")
        padded = list(code_blocks[index]) + [0] * (kb - len(code_blocks[index]))
        codeword = BL.nr_ldpc.ldpc_encode(bg, zc, padded)
        coded.extend(BL.bit_interleave(
            BL.rate_match(codeword, len(code_blocks[index]), kb, zc, e_cb, rv, bg), mod))
    assert len(coded) == e_total, f"E mismatch {len(coded)} vs {e_total}"

    scrambled = UCI.scramble(coded, layout, (rnti << 15) + n_id)
    symbols = np.array(BL.modulate(scrambled.tolist(), mod), dtype=np.complex64)
    assert len(symbols) == len(data_pos)
    if return_bits:
        return symbols, data_pos, scrambled
    return symbols, data_pos


def codeblock_layout(tb_bits, n_prb, n_syms, mod, dmrs_mask, uci=None, layout=None):
    """The codeblock segmentation and rate-matching geometry of one transmission.

    Returns `(code_blocks, bg, zc, kb, e_total, lengths)`:

    * `code_blocks` are the segmented blocks, each with its own CRC, in transmission order;
    * `bg` and `zc` are the LDPC base graph and lifting size the rate matching uses;
    * `kb` is the information length INCLUDING the filler, i.e. where the parity starts;
    * `e_total` is the number of coded bits the shared channel carries: the allocation's elements
      less the ones the control parts remove. A control part that PUNCTURES instead (HARQ-ACK of one
      or two bits) leaves `e_total` alone; the symbols it occupies are dropped from `data_pos`
      rather than from the codeword (`pusch_uci`);
    * `lengths` is the rate-matched length of each code block. The short blocks absorb the
      remainder, so the split is not `e_total / nof_blocks`.

    It exists so that a DECODER can rebuild the same geometry the encoder used: deriving it twice
    would let the two directions drift, and a drift here produces a plausible BLER rather than an
    error.

    `n_prb` is the ALLOCATION width, in the allocation's own coordinates.
    """
    import build_labels as BL
    import pusch_uci as UCI

    dmrs_syms = [s for s in range(NOF_SYMBOLS) if (dmrs_mask >> s) & 1]
    if layout is None:
        layout = UCI.data_layout(n_prb, n_syms, dmrs_syms, mod, uci)
    e_total = layout.e_total

    code_blocks = BL.segment(tb_bits)
    nof_cb = len(code_blocks)
    bg, zc, nof_cb_expected = BL.nr_ldpc.select_bg_and_zc(len(tb_bits), e_total)
    assert nof_cb_expected == nof_cb, (
        f"code block count mismatch: segmentation produced {nof_cb} but the LDPC/rate-matching "
        f"model expects {nof_cb_expected}. This usually means the allocation width is wrong "
        f"rather than the transport block.")

    max_cb = max(len(b) for b in code_blocks)
    kb_info = 22 if bg == 1 else BL.nr_ldpc._info_columns(bg, max_cb)
    for z in (2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 20, 22, 24, 26, 28, 30,
              32, 36, 40, 44, 48, 52, 56, 60, 64, 72, 80, 88, 96, 104, 112, 120, 128, 144, 160,
              176, 192, 208, 224, 240, 256, 288, 320, 352, 384):
        if kb_info * z >= max_cb:
            zc = z
            break
    # THE BLOCK SIZE IS NOT THE LADDER VALUE. 38.212's Kb ladder (6, 8, 9, 10 for BG2) exists to
    # pick Zc; the encoder's information block is then K = 10 * Zc for BG2 and 22 * Zc for BG1, whatever
    # the ladder returned. Using the ladder value as the block size shortens the block, which moves the
    # filler length and the parity start, so the whole codeword differs -- and every symbol regenerated
    # from it is wrong. It bites when B = A + 16 <= 640, because that is where the ladder returns 6 or 9;
    # above that it returns 10 and the two agree, which is why wide allocations looked healthy.
    kb = (22 if bg == 1 else 10) * zc
    e_symbols = e_total // mod
    nof_short = nof_cb - (e_symbols % nof_cb)
    lengths = [(e_symbols // nof_cb) * mod if index < nof_short
               else ((e_symbols + nof_cb - 1) // nof_cb) * mod for index in range(nof_cb)]
    return code_blocks, bg, zc, kb, e_total, lengths


def build_clean(cfg, H, X_true, data_pos, alloc_rb, dmrs_syms, dmrs_ref="prg_skip"):
    """Assemble the noiseless received grid from `H` and the re-encoded symbols.

    `H` is [symbol, subcarrier] over the whole BWP; `alloc_rb` is the allocation's PRB indices.

    The pilots are written at the amplitude the DM-RS is TRANSMITTED with, which at two CDM groups
    without data is 3 dB above the shared channel (`dmrs.amplitude_scaling`). Leaving that factor
    out makes every pilot observation 3 dB low, and because a receiver divides the pilot estimate
    by the same factor, the error survives into the channel estimate as a scale that looks like a
    training problem rather than a construction bug.
    """
    grid = np.zeros_like(H)

    # --- data REs: every subcarrier of every non-DM-RS symbol
    #
    # `data_pos` is in the allocation's own coordinates, so it must not exceed the allocation
    # width; `re_offset` shifts it to the absolute subcarrier the allocation starts at.
    alloc_width = len(alloc_rb) * NOF_SUBCARRIERS_PER_RB
    re_offset = min(alloc_rb) * NOF_SUBCARRIERS_PER_RB if alloc_rb else 0
    for k, (symbol, subcarrier) in enumerate(data_pos):
        assert subcarrier < alloc_width, (
            f"data position {subcarrier} is outside the allocation width {alloc_width}")
        grid[symbol, subcarrier + re_offset] = H[symbol, subcarrier + re_offset] * X_true[k]

    # --- DM-RS symbols: pilots on the even offsets, reserved odd offsets left at zero
    pilots = DV.pilot_subcarriers(alloc_rb, dmrs_type=int(cfg["dmrs_type"]))
    nid = int(cfg["dmrs_scrambling_id"])
    nscid = int(cfg["dmrs_n_scid"])
    slot_index = int(cfg["slot"]) % SLOTS_PER_FRAME_30KHZ
    amplitude = DV.amplitude_scaling(int(cfg["dmrs_nof_cdm_groups_without_data"]))

    #   "prg_skip" - correct: the PRG advances 6 RE per PRB from reference point k=0 up to the
    #                first allocated PRB, so the sequence depends on absolute PRB position.
    #                Verified against the chain's own dump, including an allocation at PRB 8.
    #   "no_skip"  - starts the sequence at the allocation. Wrong for any allocation not
    #                beginning at PRB 0 and correct-by-accident for one that does, which is
    #                exactly why this is a switch rather than a comment.
    for symbol in dmrs_syms:
        alloc = alloc_rb if dmrs_ref == "prg_skip" else list(range(len(alloc_rb)))
        sequence = DV.sequence(slot_index, symbol, nid, nscid, alloc)
        assert len(sequence) == len(pilots), f"dmrs length {len(sequence)} vs {len(pilots)} pilots"
        grid[symbol, pilots] = H[symbol, pilots] * sequence * amplitude
    return grid


def read_config(prefix):
    """Read a capture's `key=value` metadata sidecar."""
    cfg = {}
    with open(prefix + ".txt") as handle:
        for line in handle:
            if "=" in line:
                key, _, value = line.partition("=")
                cfg[key.strip()] = value.strip()
    return cfg


def add_noise(grid, snr_db, seed):
    """Add white Gaussian noise at a target SNR, anchored on the grid's own signal power.

    The anchor is the mean power over the non-zero REs, so the chain's reported SNR matches the
    requested one (verified: the chain's `noise_variance` tracks this construction).
    """
    signal_power = float(np.mean(np.abs(grid[np.abs(grid) > 0]) ** 2))
    sigma2 = signal_power / (10.0 ** (snr_db / 10.0))
    rng = np.random.default_rng(seed)
    noise = rng.standard_normal(grid.shape) + 1j * rng.standard_normal(grid.shape)
    return grid + noise * np.sqrt(sigma2 / 2.0), sigma2
