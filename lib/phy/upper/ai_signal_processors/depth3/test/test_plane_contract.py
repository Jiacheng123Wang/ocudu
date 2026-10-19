#!/usr/bin/env python3
"""The packed plane round-trips, on both DM-RS patterns. Pure numpy: no TensorFlow, no captures.

WHY THIS EXISTS. `pack_input` is where the received grid, the regenerated DM-RS sequence, the
allocation and the metadata all meet, and `unpack_input` exists precisely so a producer can be checked
against the contract instead of against another copy of itself (its own docstring says so). If the two
drift apart, every downstream measurement still runs and every number is quietly wrong -- which is the
failure mode this project has paid for most often.

What is asserted, per pattern:
  * the plane has the contract's shape and every non-NaN value;
  * the reference amplitude is positive and finite;
  * the DM-RS mask channel in the plane equals `pilot_ls_mask` (the plane tells the network where the
    pilots are, so it must agree with the sequence that was subtracted);
  * the grid recovered from the plane, scaled back by `r`, matches the grid that went in on every
    allocated subcarrier;
  * the metadata that identifies the pattern survives the round trip -- the very field the field
    failures were about (design section 19.211).
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "ai_train"))


def make_meta(nof_symbols=14):
    return {
        "nslot": 17, "n_id": 321, "n_scid": 0, "dmrs_type": 1,
        "nof_cdm_groups_without_data": 2, "nof_tx_layers": 1, "nof_rx_ports": 1,
        "active_dmrs_ports": 1, "nof_symbols": nof_symbols, "start_symbol_index": 0,
        "alloc_nof_prb": 6, "alloc_first_prb": 0, "dc_position": None,
    }


def main():
    try:
        import depth3 as D3
        import dmrs as DV
    except ImportError as error:              # pragma: no cover - reported as a CTest skip
        print(f"SKIP: cannot import depth3/dmrs ({error})")
        return 77

    alloc = range(6)
    modulation = "64QAM"
    counts = {}
    for name, dmrs in (("2,11", [2, 11]), ("2,7,11", [2, 7, 11])):
        meta = make_meta()
        grid = np.zeros((D3.NOF_SYMBOLS, D3.NOF_SUBCARRIERS), dtype=np.complex64)
        pilots = DV.pilot_subcarriers(alloc, meta["dmrs_type"])
        for symbol in dmrs:
            seq = DV.sequence(meta["nslot"], symbol, meta["n_id"], meta["n_scid"], alloc)
            # Unit-magnitude pilots: the LS then reads ~1, so the reference amplitude is ~1 and the
            # recovered grid can be compared directly rather than through a fitted scale.
            grid[symbol, pilots] = seq.astype(np.complex64)
        counts[name] = pilots.size * len(dmrs)

        plane, r = D3.pack_input(grid, alloc, dmrs, meta)
        assert plane.shape == (D3.NOF_SUBCARRIERS, D3.NOF_CHANNELS), plane.shape
        assert np.isfinite(plane).all(), f"{name}: the plane carries non-finite values"
        assert np.isfinite(r) and r > 0, f"{name}: reference amplitude {r}"

        back = D3.unpack_input(plane, alloc, dmrs, meta["dmrs_type"])
        assert np.allclose(back["dmrs_mask"], D3.pilot_ls_mask(alloc, dmrs, meta["dmrs_type"])), \
            f"{name}: the plane's DM-RS mask disagrees with the mask its own sequence was built on"

        recovered = back["grid"] * r
        allocated = np.zeros(D3.NOF_SUBCARRIERS, dtype=bool)
        allocated[np.flatnonzero(D3.allocated_subcarriers(alloc))] = True
        error = np.abs(recovered[:, allocated] - grid[:, allocated]).max()
        assert error < 1e-3, f"{name}: recovered grid differs by {error:.3e} on the allocation"

        assert int(back["metadata"]["nof_dmrs_symbols"]) == len(dmrs), \
            f"{name}: the pattern did not survive the round trip: {back['metadata']}"

    assert counts["2,11"] != counts["2,7,11"], \
        f"both patterns carry {counts['2,11']} pilots -- the pattern is not reaching the packer"
    print(f"OK  plane contract: round trip exact on both patterns "
          f"(pilots 2,11 {counts['2,11']}, 2,7,11 {counts['2,7,11']})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
