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


def encode_symbols(tb_bits, n_prb, n_syms, mod, dmrs_mask, n_id, rnti, rv, n_layers=1):
    """Re-encode a transport block into the transmitted data symbols.

    Follows the same primitive chain as `build_labels.build_label` (segment -> LDPC encode ->
    rate match -> bit interleave -> scramble -> modulate) and imports those primitives from it,
    so the two cannot drift. It exists because `build_label` returns a SMOOTHED channel estimate
    rather than the transmitted symbols.

    `n_prb` is the ALLOCATION width: the returned positions are in the allocation's own
    coordinates, starting at 0. Passing the BWP width instead over-counts the data REs, which
    changes the rate-matching length; the code-block assertion below catches that.

    Returns (symbols, data_pos), where data_pos is a list of (symbol, subcarrier) pairs.
    """
    import build_labels as BL

    n_sc = n_prb * NOF_SUBCARRIERS_PER_RB
    dmrs_syms = [s for s in range(NOF_SYMBOLS) if (dmrs_mask >> s) & 1]
    data_pos = [(s, sc) for s in range(n_syms) if s not in dmrs_syms for sc in range(n_sc)]
    e_total = len(data_pos) * mod

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
    kb = kb_info * zc
    e_symbols = e_total // mod
    nof_short = nof_cb - (e_symbols % nof_cb)

    coded = []
    for index in range(nof_cb):
        if kb < len(code_blocks[index]):
            raise ValueError(f"K_b {kb} < code block {len(code_blocks[index])}")
        padded = list(code_blocks[index]) + [0] * (kb - len(code_blocks[index]))
        codeword = BL.nr_ldpc.ldpc_encode(bg, zc, padded)
        # Code blocks are rate-matched to slightly different lengths: the short ones absorb the
        # remainder so the concatenation lands exactly on the allocation's bit count.
        e_cb = ((e_symbols // nof_cb) * mod if index < nof_short
                else ((e_symbols + nof_cb - 1) // nof_cb) * mod)
        coded.extend(BL.bit_interleave(
            BL.rate_match(codeword, len(code_blocks[index]), kb, zc, e_cb, rv, bg), mod))
    assert len(coded) == e_total, f"E mismatch {len(coded)} vs {e_total}"

    scrambled = BL.scramble(coded, (rnti << 15) + n_id)
    symbols = np.array(BL.modulate(scrambled, mod), dtype=np.complex64)
    assert len(symbols) == len(data_pos)
    return symbols, data_pos


def build_clean(cfg, H, X_true, data_pos, alloc_rb, dmrs_syms, dmrs_ref="prg_skip"):
    """Assemble the noiseless received grid from `H` and the re-encoded symbols.

    `H` is [symbol, subcarrier] over the whole BWP; `alloc_rb` is the allocation's PRB indices.
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
        grid[symbol, pilots] = H[symbol, pilots] * sequence
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
