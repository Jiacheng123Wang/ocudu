#!/usr/bin/env python3
"""The control payload's element map, against the numbers the chain produced.

WHY IT EXISTS. The shared channel does not occupy every element of a data symbol once the reception
carries control information, and both the length of its codeword and the position the descrambler
reads at change with it. Two failures follow from getting that wrong, and neither is visible in a
shape: a codeword that correlates with nothing (so the data-aided channel is noise and the sample is
rejected), and a label plane that scatters the stored bits over elements the reception did not
carry them on (so a trained model is scored against labels that are not there). The first is what
the measured element counts below pin down; the second is what the scatter/gather/mask assertions
pin down.

The numbers are not invented. `pusch_uci` reproduces `get_ulsch_information`
(`lib/ran/pusch/ulsch_info.cpp`) and `ulsch_demultiplex_impl::configure_current_ofdm_symbol`; the
expected values are the ones the chain was measured to produce on a 43 PRB QPSK reception carrying
a four-bit CSI Part 1 report (97 elements, every fifth element of the first data symbol, a codeword
of 12190 bits) and on a 41 PRB one (103 elements, 11602 bits).
"""
import os
import sys

import numpy as np

# depth3.py and pusch_uci.py live in ../ai_train/, beside this test's parent.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "ai_train"))


def sidecar(**overrides):
    """A capture sidecar's control description, with the defaults a QPSK leg records."""
    cfg = {
        "nof_symbols": "14",
        "nof_tx_layers": "1",
        "nof_harq_ack": "0",
        "nof_csi_part1": "0",
        "nof_csi_part2_entries": "0",
        "alpha_scaling": "0",
        "beta_offset_harq_ack": "0",
        "beta_offset_csi_part1": "0",
        "beta_offset_csi_part2": "0",
        "target_code_rate": "120",
    }
    cfg.update({key: str(value) for key, value in overrides.items()})
    return cfg


def main():
    try:
        import depth3 as D3
        import dd_label as DL
        import pusch_uci as UCI
    except ImportError as error:              # pragma: no cover - reported as a CTest skip
        print(f"SKIP: cannot import the layout modules ({error})")
        return 77

    # ---- the segmentation the element formulas divide by -------------------------------------
    # 1480 payload bits at R = 120/1024: BG2, one code block, K = 10 * 160 = 1600 bits.
    bg, nof_cb, nof_bits_per_cb = UCI.sch_segmentation(1480, 120)
    assert (bg, nof_cb, nof_bits_per_cb) == (2, 1, 1600), (bg, nof_cb, nof_bits_per_cb)
    # 1416 payload bits: the same base graph, a smaller block.
    bg, nof_cb, nof_bits_per_cb = UCI.sch_segmentation(1416, 120)
    assert (bg, nof_cb, nof_bits_per_cb) == (2, 1, 1440), (bg, nof_cb, nof_bits_per_cb)

    # ---- CSI Part 1, 43 PRB: 97 elements, every fifth element of the first data symbol --------
    uci = UCI.from_capture(sidecar(nof_csi_part1=4, alpha_scaling=1, beta_offset_harq_ack=1,
                                   beta_offset_csi_part1=6.25),
                           np.zeros(1480, dtype=np.uint8), 43, [2, 11], 1, 2)
    assert (uci.nof_ack_re, uci.nof_ack_rvd_re, uci.nof_csi1_re) == (0, 8, 97), uci
    layout = UCI.data_layout(43, 14, [2, 11], 2, uci)
    assert layout.e_total == 12190, layout.e_total
    assert layout.nof_re_ul_sch == 6192 - 97, layout.nof_re_ul_sch
    assert layout.nof_punctured == 0
    csi1 = layout.re_sets.csi1
    assert [symbol for symbol in range(14) if csi1[symbol].any()] == [0], "CSI is not in symbol 0"
    assert np.array_equal(np.flatnonzero(csi1[0]), np.arange(0, 481, 5)), "CSI positions"
    assert np.array_equal(np.flatnonzero(~layout.re_sets.ulsch[0]), np.arange(0, 481, 5)), \
        "the shared channel did not lose exactly the CSI elements"
    # The scrambling index is the element's position in the FULL allocation, so the first shared
    # channel element of symbol 0 is element 1 and the last element of a symbol is offset by the
    # 97 the control part took from the start of the codeword's index space.
    assert layout.scramble_re[:3] == [1, 2, 3], layout.scramble_re[:3]
    assert layout.scramble_re[-1] == 6192 - 1, layout.scramble_re[-1]

    # ---- CSI Part 1, 41 PRB ---------------------------------------------------------------
    uci41 = UCI.from_capture(sidecar(nof_csi_part1=4, alpha_scaling=1, beta_offset_harq_ack=1,
                                     beta_offset_csi_part1=6.25),
                             np.zeros(1416, dtype=np.uint8), 41, [2, 11], 1, 2)
    assert uci41.nof_csi1_re == 103, uci41
    layout41 = UCI.data_layout(41, 14, [2, 11], 2, uci41)
    assert layout41.e_total == 11602, layout41.e_total

    # ---- HARQ-ACK of four bits: removed, and the codeword shortens --------------------------
    uci_ack = UCI.from_capture(sidecar(nof_harq_ack=4, alpha_scaling=1, beta_offset_harq_ack=6.25),
                               np.zeros(1480, dtype=np.uint8), 43, [2, 11], 1, 2)
    assert (uci_ack.nof_ack_re, uci_ack.nof_ack_rvd_re) == (97, 0), uci_ack
    ack_layout = UCI.data_layout(43, 14, [2, 11], 2, uci_ack)
    assert ack_layout.e_total == 12190, ack_layout.e_total
    # `get_ulsch_demultiplex_l1`: HARQ-ACK starts at the first data symbol AFTER the first DM-RS
    # symbol, which is symbol 3 for DM-RS at 2 and 11.
    ack_symbols = [symbol for symbol in range(14) if ack_layout.re_sets.ack[symbol].any()]
    assert ack_symbols == [3], ack_symbols
    assert np.array_equal(np.flatnonzero(ack_layout.re_sets.ack[3]), np.arange(0, 481, 5))
    assert ack_layout.nof_punctured == 0, "a removed part must not be reported as puncturing"

    # ---- HARQ-ACK of one bit: punctured, and the codeword does not shorten ------------------
    uci_one = UCI.from_capture(sidecar(nof_harq_ack=1, alpha_scaling=1,
                                       beta_offset_harq_ack=20),
                               np.zeros(1480, dtype=np.uint8), 43, [2, 11], 1, 2)
    assert (uci_one.nof_ack_re, uci_one.nof_ack_rvd_re) == (78, 155), uci_one
    one_layout = UCI.data_layout(43, 14, [2, 11], 2, uci_one)
    assert one_layout.e_total == 12384, one_layout.e_total
    assert one_layout.nof_punctured == 78, one_layout.nof_punctured
    assert len(one_layout.data_pos) == one_layout.nof_re_ul_sch - 78
    # The punctured elements are a subset of the reserved ones, striding over them.
    punctured = [one_layout.ulsch_pos[index]
                 for index, keep in enumerate(one_layout.keep) if not keep]
    assert punctured == [(3, subcarrier) for subcarrier in range(0, 232, 3)], punctured[:4]
    # A punctured element consumes a codeword group and carries no symbol, so the kept bits are
    # the codeword less those groups.
    assert int(one_layout.keep_bits.sum()) == 12384 - 78 * 2, int(one_layout.keep_bits.sum())

    # ---- CSI Part 2 is refused, not assumed absent -----------------------------------------
    try:
        UCI.from_capture(sidecar(nof_csi_part1=4, nof_csi_part2_entries=3, alpha_scaling=1),
                         np.zeros(1480, dtype=np.uint8), 43, [2, 11], 1, 2)
    except ValueError as error:
        assert "part 2" in str(error).lower(), error
    else:
        raise AssertionError("a capture with CSI part 2 was accepted")

    # ---- the map does not depend on the modulation order ------------------------------------
    # Every count the demultiplexer works with is scaled by Qm and divided by it again.
    for modulation, qm in (("QPSK", 2), ("16QAM", 4), ("256QAM", 8)):
        same = UCI.data_layout(43, 14, [2, 11], qm, uci)
        assert same.data_pos == layout.data_pos, modulation
        assert same.scramble_re == layout.scramble_re, modulation
        assert same.e_total == layout.e_total // 2 * qm, (modulation, same.e_total)

    # ---- the label plane walks the same elements the symbols do -----------------------------
    alloc = list(range(0, 43))
    full = D3.data_re_mask(alloc, [2, 11])
    assert int(full.sum()) == 43 * 12 * 12, int(full.sum())
    controlled = D3.data_re_mask(alloc, [2, 11], uci)
    assert int(full.sum() - controlled.sum()) == 97, int(full.sum() - controlled.sum())
    subcarriers, symbols = D3.data_re_indices(alloc, [2, 11], uci)
    assert subcarriers.size == int(controlled.sum())
    rng = np.random.default_rng(20261102)
    bits = rng.integers(0, 2, size=subcarriers.size * 2).astype(np.float32)
    plane = D3.label_plane(bits, alloc, [2, 11], "QPSK", uci)
    assert np.array_equal(D3.llr_gather(plane, alloc, [2, 11], "QPSK", uci), 1.0 - bits)
    weights = D3.loss_weights(alloc, [2, 11], "QPSK", uci)
    assert int(weights.sum()) == bits.size, int(weights.sum())
    # The control elements carry no label, which is the failure a label path that ignored the
    # payload produces and nothing else catches.
    csi_positions = np.flatnonzero(layout.re_sets.csi1[0])
    assert not plane[csi_positions, 0:2].any(), "a label landed on a CSI element"
    # The stored bits are the TRANSMITTED ones, so the conditioned layout's scatter agrees too.
    conditioned = D3.label_plane_cond(bits, alloc, [2, 11], "QPSK", uci)
    assert D3.llr_gather_cond(conditioned, alloc, [2, 11], "QPSK", uci).size == bits.size

    # ---- without control information the scrambling is the contiguous sequence --------------
    # This is what keeps every control-free reception on the code path it had: the same codeword,
    # the same symbols and the same bits, not merely the same length.
    cinit = (17940 << 15) + 1
    coded = rng.integers(0, 2, size=51 * 12 * 12 * 2 + 2000).astype(np.uint8)
    for n_prb, uci_empty in ((43, UCI.EMPTY), (41, UCI.EMPTY), (51, UCI.EMPTY)):
        plain = UCI.data_layout(n_prb, 14, [2, 11], 2, uci_empty)
        assert plain.e_total == n_prb * 12 * 12 * 2, (n_prb, plain.e_total)
        assert plain.data_pos == [(symbol, subcarrier)
                                  for symbol in range(14) if symbol not in (2, 11)
                                  for subcarrier in range(n_prb * 12)]
        word = coded[:plain.e_total]
        assert np.array_equal(UCI.scramble(word, plain, cinit), DL.scramble(word, cinit))

    print(f"OK  control map: CSI1 43 PRB {layout.e_total} bits over {layout.nof_re_ul_sch} elements, "
          f"41 PRB {layout41.e_total}, HARQ-ACK 4 {ack_layout.e_total}, 1 {one_layout.e_total} "
          f"with {one_layout.nof_punctured} punctured; label plane and mask agree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
