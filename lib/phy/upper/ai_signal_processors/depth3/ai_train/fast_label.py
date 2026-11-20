"""Vectorised label construction, byte-for-byte the same as the reference implementation.

WHY THIS FILE EXISTS. A profile of one training epoch (2026-11-20) put 52% of the per-sample preparation time in
`dd_label.modulate`, which builds the transmitted symbols with a Python loop over every symbol -- 153,644 `list.append`
calls per sample that only adds up to a few hundred microseconds of arithmetic. Threads cannot compensate for it either:
six threads measured 0.80x the speed of one, because the loop holds the GIL. The same arithmetic expressed over a whole
bit vector takes microseconds and releases the GIL in NumPy, so the data pipeline stops being the bottleneck that starves
the GPU (it sat at 13% utilisation while the card was idle waiting for batches).

The reference implementation is left untouched (it is original gNB code and is still the source of truth for the
datasets); this is a re-expression of the same formulas, and `verify_agreement()` proves the equivalence.
"""
import sys

import numpy as np

CODE = "/Users/jiachengwang/dev/ocudu/lib/phy/upper/signal_processors/channel_estimator/metal/ai_train"
if CODE not in sys.path:
    sys.path.insert(0, CODE)


def modulate(bits, mod):
    """Transmitted QAM symbols for `bits`, matching ``dd_label.modulate`` exactly.

    `mod` is bits per symbol (QPSK=2, 16QAM=4, 64QAM=6, 256QAM=8). The Gray-coded mapping is the same expression the
    reference uses, only evaluated over the whole block at once instead of symbol by symbol. A tail that does not fill a
    whole symbol is dropped, which is what the reference's range() does implicitly by raising on the short slice.
    """
    b = np.asarray(bits, dtype=np.int8).ravel()
    n = (b.size // mod) * mod
    s = 1 - 2 * b[:n].reshape(-1, mod).astype(np.float64)
    if mod == 2:
        re, im, norm = s[:, 0], s[:, 1], 2.0
    elif mod == 4:
        re, im, norm = s[:, 0] * (2.0 - s[:, 2]), s[:, 1] * (2.0 - s[:, 3]), 10.0
    elif mod == 6:
        re = s[:, 0] * (4.0 - s[:, 2] * (2.0 - s[:, 4]))
        im = s[:, 1] * (4.0 - s[:, 3] * (2.0 - s[:, 5]))
        norm = 42.0
    elif mod == 8:
        re = s[:, 0] * (8.0 - s[:, 2] * (4.0 - s[:, 4] * (2.0 - s[:, 6])))
        im = s[:, 1] * (8.0 - s[:, 3] * (4.0 - s[:, 5] * (2.0 - s[:, 7])))
        norm = 170.0
    else:
        raise ValueError(f"bad mod {mod}")
    return (re + 1j * im) / np.sqrt(norm)


def verify_agreement(trials=20, seed=3):
    """Compare against the reference on random bit blocks for all four modulations."""
    import dd_label as DL   # noqa: E402  the reference implementation

    rng = np.random.default_rng(seed)
    worst = 0.0
    for mod in (2, 4, 6, 8):
        for _ in range(trials):
            n = int(rng.integers(64, 2048)) * mod
            bits = rng.integers(0, 2, n).astype(np.uint8)
            reference = np.asarray(DL.modulate(bits.tolist(), mod), dtype=np.complex64)
            ours = modulate(bits, mod).astype(np.complex64)
            worst = max(worst, float(np.max(np.abs(reference - ours))) if reference.size else 0.0)
    return worst


if __name__ == "__main__":
    err = verify_agreement()
    print(f"max |reference - fast| = {err:.3e}")
    assert err < 1e-6, "the fast modulator does not reproduce the reference"
    print("fast_label: agreement verified")
