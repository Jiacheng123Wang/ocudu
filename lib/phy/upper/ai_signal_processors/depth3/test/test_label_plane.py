#!/usr/bin/env python3
"""The label plane and its inverse, for BOTH DM-RS patterns. No TensorFlow, no captures, milliseconds.

WHY THIS EXISTS. Three separate defects in this project were all "the pattern the label assumes is not
the pattern the reception has": training on 2,11 only and then meeting 2,7,11 in the field cost weeks
(design section 19.211), and the label scatter is the single place where that assumption is encoded.
So this test walks the same bits through both patterns and asserts:

  * every bit lands at `symbol * COND_STRIDE + bit` and nowhere else;
  * `llr_gather_cond` recovers exactly the bits that `label_plane_cond` scattered -- the two are
    documented as inverses, and an off-by-one there would silently train against shifted labels;
  * `llr_valid_mask_cond` covers exactly the scattered positions, so the loss cannot average over
    don't-care channels;
  * the two patterns carry DIFFERENT numbers of data resource elements, which is the fact the
    field failures were made of. A regression that silently reused one pattern's mask would pass
    everything except this assertion.
"""
import os
import sys

import numpy as np

# depth3.py lives in ../ai_train/, beside this test's parent: the package layout is
# depth3/{ai_train,test,depth3_modcond_*} (design section 19.226).
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "ai_train"))


def main():
    try:
        import depth3 as D3
    except ImportError as error:              # pragma: no cover - reported as a CTest skip
        print(f"SKIP: cannot import depth3 ({error})")
        return 77

    allocation = range(6)          # PRB 0..5 (the helpers take an iterable of PRBs)
    start = 0
    modulation = "64QAM"
    qm = D3.BITS_PER_SYMBOL[modulation]
    rng = np.random.default_rng(20261019)
    patterns = [("2,11", [2, 11]), ("2,7,11", [2, 7, 11])]

    counts = {}
    for name, dmrs in patterns:
        subcarriers, symbols = D3.data_re_indices(allocation, dmrs)
        counts[name] = subcarriers.size
        bits = rng.integers(0, 2, subcarriers.size * qm).astype(np.float32)

        plane = D3.label_plane_cond(bits, allocation, dmrs, modulation)
        assert plane.shape == (D3.NOF_SUBCARRIERS, D3.llr_channels_cond()), plane.shape

        # 1. every bit at symbol*stride+bit, nothing elsewhere
        expected = np.zeros_like(plane)
        element = np.arange(bits.size) // qm
        bit = np.arange(bits.size) % qm
        expected[subcarriers[element], symbols[element] * D3.COND_STRIDE + bit] = 1.0 - bits
        assert np.array_equal(plane, expected), f"{name}: the scatter does not match the documented layout"

        # 2. the gather is its inverse
        back = D3.llr_gather_cond(plane, allocation, dmrs, modulation)
        assert np.array_equal(back.astype(np.float32), (1.0 - bits).astype(np.float32)), \
            f"{name}: llr_gather_cond does not invert label_plane_cond"

        # 3. the valid mask covers exactly the scattered positions. Note: compared against a POSITION
        # mask, not against `expected > 0` -- a label of 0 is a legitimate value there, so testing the
        # values would flag half the plane as "uncovered" (this assertion was written wrong first).
        valid = D3.llr_valid_mask_cond(allocation, dmrs, modulation)
        where = np.zeros(expected.shape, dtype=bool)
        where[subcarriers[element], symbols[element] * D3.COND_STRIDE + bit] = True
        assert valid.shape == where.shape, (valid.shape, where.shape)
        assert np.array_equal(valid, where), \
            f"{name}: the valid mask and the scattered positions disagree " \
            f"({int((valid & ~where).sum())} extra, {int((where & ~valid).sum())} missing)"

    # 4. the two patterns are not interchangeable, and 2,7,11 carries fewer data REs
    assert counts["2,11"] != counts["2,7,11"], \
        f"both patterns carry {counts['2,11']} data REs -- the pattern is not reaching the layout"
    assert counts["2,7,11"] < counts["2,11"], (counts, "an extra DM-RS symbol must cost data REs")
    print(f"OK  label plane: 2,11 {counts['2,11']} data REs, 2,7,11 {counts['2,7,11']}; "
          f"scatter, gather and valid mask agree on both")
    return 0


if __name__ == "__main__":
    sys.exit(main())
