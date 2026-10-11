#!/usr/bin/env python3
"""PUSCH DM-RS sequence generation, verified against the chain's own output.

Why this module exists
----------------------
Every label builder in this directory eventually needs the DM-RS reference symbols: the
pilots are the only REs whose transmitted value is known a priori, so they anchor both the
channel estimate and any synthetic grid. Getting the sequence subtly wrong (a swapped bit
pair, a global sign, or the wrong PRG start) produces labels that look plausible and are
worthless, so the conventions here were not derived from the specification alone - they were
settled by reproducing sequences dumped from the chain itself.

The chain dumps what `dmrs_pusch_estimator_impl::sequence_generation` actually produced when
`OCUDU_HELENA_DMRS_DUMP` names a directory (see that function; the dump is off in a release
build). `dmrs_verify.py` in the llr_ai_detection work area replays such a dump against this
module and reports a point-by-point comparison.

Conventions, each verified against dumps
----------------------------------------
The evidence is six dumped sequences: two symbols each from a 4 PRB allocation starting at
PRB 0, a 3 PRB allocation starting at PRB 8, and a replay of the same 4 PRB case. All six are
reproduced to float32 rounding. Every convention below was determined by brute-forcing all
plausible alternatives against those six; exactly one hypothesis reproduces them.

1. The PRG initial state is, for normal CP (nsymb = 14),

       c_init = ((nsymb*nslot + symbol + 1)*(2*nidnscid + 1)*2^17
                 + (2*nidnscid + nscid)) mod 2^31

   which is TS38.211 7.4.1.1.1 and matches `dmrs_pusch_estimator_impl.cpp:103`.

2. The Gold sequence uses the recurrence the unit tests check the generator against
   (`pseudo_random_generator_test.cpp:83-95`), with Nc = 1600:

       x1[0] = 1, x1[n] = 0 otherwise;  x2[n] = bit n of c_init
       x1[n+31] = x1[n+3] ^ x1[n]
       x2[n+31] = x2[n+3] ^ x2[n+2] ^ x2[n+1] ^ x2[n]
       c[n]     = x1[n+Nc] ^ x2[n+Nc]

   The seeding is *not* bit-reversed. It is easy to think it must be, because
   `pseudo_random_generator_impl::init` calls `get_reverse(c_init)` - but that generator keeps
   x(0) in the most significant bit of its state word and reverses its registers too, so the
   reversal cancels. The emitted bit order is what matters.

3. RE k of the allocation consumes the Gold bits (b[2k], b[2k+1]) as (real, imaginary):

       s[k] = (1 - 2*b[2k]) / sqrt(2) + j * (1 - 2*b[2k+1]) / sqrt(2)

   A set bit gives -1. Both the pair order and the global sign were wrong in early attempts.
   Note the global sign is *invisible* in any product of two sequence values
   (`s1 * conj(s0)` is unchanged by flipping both signs), which is why comparing sequence
   ratios against a capture could never have settled it - only a dump could.

4. The PRG does not begin at the allocation. It advances 6 RE (= 12 Gold bits) for every PRB
   between reference point k = 0 and the first allocated PRB (`dmrs_helper.cpp:64`), so a
   contiguous allocation starting at PRB 8 consumes bits that begin 96 RE into the sequence.
   A gap between allocated PRBs advances it the same way, so the sequence is a function of
   absolute PRB position rather than of allocation order.

   This is the convention that a hand-written generator is most likely to get wrong in the
   other direction: with reference point 0 the skip is *exactly* the allocation's own start,
   so an allocation at PRB 0 works either way and hides the bug.

Validation
----------
    python3 dmrs_verify.py --dump <dir>      # regenerate and compare against a chain dump
    python3 dmrs_verify.py <capture-prefix>  # informational check against a capture
"""

import numpy as np

NOF_SYMBOLS_PER_SLOT_NORMAL_CP = 14
NOF_DMRS_RE_PER_PRB_TYPE1 = 6
NC = 1600
AMPLITUDE = 1.0 / np.sqrt(2.0)


def c_init(nslot: int, symbol: int, nidnscid: int, nscid: int) -> int:
    """PRG initial state for one DM-RS symbol (TS38.211 7.4.1.1.1)."""
    return ((NOF_SYMBOLS_PER_SLOT_NORMAL_CP * nslot + symbol + 1) * (2 * nidnscid + 1) * (1 << 17)
            + (2 * nidnscid + nscid)) % (1 << 31)


def gold_sequence(c_init_value: int, length: int) -> np.ndarray:
    """The first `length` bits c(n) of the length-31 Gold sequence."""
    total = NC + length
    x1 = np.zeros(total + 31, dtype=np.uint8)
    x2 = np.zeros(total + 31, dtype=np.uint8)
    for n in range(31):
        x2[n] = (c_init_value >> n) & 1
    x1[0] = 1

    for n in range(total):
        x1[n + 31] = (x1[n + 3] ^ x1[n]) & 1
        x2[n + 31] = (x2[n + 3] ^ x2[n + 2] ^ x2[n + 1] ^ x2[n]) & 1

    return (x1[NC:NC + length] ^ x2[NC:NC + length]).astype(np.uint8)


def sequence(nslot: int, symbol: int, nidnscid: int, nscid: int, alloc_prb) -> np.ndarray:
    """DM-RS reference symbols for one symbol, in allocation order.

    `alloc_prb` is the allocation as PRB indices inside the BWP. It may contain gaps: the
    generator advances over them, so the result is a function of absolute PRB position.

    Returns 6 complex values per allocated PRB, ordered by PRB and then by even subcarrier
    offset (0, 2, 4, 6, 8, 10) - the order `dmrs_helper.cpp` fills the buffer in, and the order
    a caller must use when writing them into a grid.
    """
    rbs = sorted(int(r) for r in alloc_prb)
    if not rbs:
        return np.zeros(0, dtype=np.complex128)

    # Generate the whole span once and index into it: equivalent to the per-interval
    # "advance, then generate" loop in `dmrs_helper.cpp:61-78`, and much simpler to get right.
    span = gold_sequence(c_init(nslot, symbol, nidnscid, nscid), 2 * NOF_DMRS_RE_PER_PRB_TYPE1 * (rbs[-1] + 1))
    span = span.astype(np.int64)

    re_index = np.concatenate([np.arange(r * NOF_DMRS_RE_PER_PRB_TYPE1,
                                        (r + 1) * NOF_DMRS_RE_PER_PRB_TYPE1) for r in rbs])
    return ((1.0 - 2.0 * span[2 * re_index]) * AMPLITUDE
            + 1j * (1.0 - 2.0 * span[2 * re_index + 1]) * AMPLITUDE)


def pilot_subcarriers(alloc_prb, dmrs_type: int = 1) -> np.ndarray:
    """Subcarrier indices inside the BWP that carry DM-RS, in the order `sequence` returns them.

    Type 1 with two CDM groups without data occupies all six even offsets; the two groups are
    separated by an orthogonal cover code rather than by position (measured on the captures:
    the odd offsets of a DM-RS symbol carry ~1e-4 of the pilot power).
    """
    offsets = (0, 2, 4, 6, 8, 10) if dmrs_type == 1 else (0, 1, 6, 7)
    return np.concatenate([np.arange(r * 12, r * 12 + 12)[list(offsets)] for r in sorted(alloc_prb)])
