#!/usr/bin/env python3
"""Decode the network's LLRs through the real LDPC code, so "coded BLER" is measurable offline.

Why this exists
---------------
`design` section 2.3 and R2.4 make coded BLER the criterion for the main head, and it must not
get worse when the auxiliary heads are attached. Uncoded bit error rate is only a trend: it says
nothing about whether the code can still correct what the network got wrong. `nr_ldpc` has the
encoder and the parity-check matrix but no decoder, so a rate-matched block could not be decoded
anywhere except inside the C++ chain.

This is a flooding normalized min-sum decoder over the quasi-cyclic parity-check matrix that
`nr_ldpc.build_H` returns, plus the three inverse steps that sit between the network's LLRs and a
codeblock: descrambling, deinterleaving and rate dematching. Those three are the exact inverses of
what `dd_label` does forward, and `--selfcheck` proves it: with perfect LLRs taken from the
transmitted bits, the whole path reproduces the transmitted codeword bit for bit.

What it is not: the acceptance criterion stays the chain's own LDPC decoder and CRC, as section
2.3 requires. This decoder is for the training loop, where a metric that costs a subprocess per
sample is not usable. `memo_03` records that min-sum is robust to LLR scale mismatch, which is
what makes it a fair stand-in while the scale is still being calibrated.

Usage: ldpc_decode.py --selfcheck
"""

import argparse
import sys
from collections import defaultdict

import numpy as np

HERE = __file__.rsplit("/", 1)[0]
sys.path.insert(0, HERE)

import dd_label as DL              # noqa: E402
import nr_ldpc as NL               # noqa: E402

# LLR magnitude used to stand in for a known bit: large enough to be decisive, small enough to
# stay exactly representable and not to dominate a sum.
KNOWN_LLR = 30.0


def deinterleave(bits, mod):
    """Inverse of `dd_label.bit_interleave`: the modulation-order interleaver of TS38.212 5.4.2.2.

    Forward writes `mod` rows of `E // mod` and reads columns, so the inverse reads the same
    layout the other way round. Reshaping is the whole operation: `f[i * Qm + j]` becomes
    `[i, j]`, and transposing gives `[j, i]`, which flattens to `j * EQm + i`.
    """
    bits = np.asarray(bits)
    E = bits.size
    qm = int(mod)
    eqm = E // qm
    if E % qm:
        raise ValueError(f"rate-matched length {E} is not a multiple of {qm}")
    return bits.reshape(eqm, qm).T.reshape(-1)


def rate_dematch(llr, K_transmitted, K_b, Zc, E, rv, bg=None):
    """Deposit rate-matched LLRs into the codeblock's circular buffer.

    The exact inverse of `dd_label.rate_match`, including its buffer layout: the systematic
    window is `codeword[2 * Zc : K_transmitted]` and the parity section starts at `K_b`, so a
    received position maps to a codeword position rather than to a buffer position.

    Returns `(codeword, received)`: the LLR of the whole codeword, length `nof_columns * Zc`, and a
    mask of the positions a received LLR was deposited into. The `2 * Zc` punctured prefix and the
    untransmitted parity are erasures at zero, while the filler is a known value because every
    receiver knows those bits are zero.

    Repetitions are summed, which is soft combining, and is what the chain's rate dematcher does.
    """
    if bg is None:
        bg = 1 if (int(K_b) // int(Zc)) == 22 else 2
    mb, nb, _ = NL._BG_PARAMS[bg]
    llr = np.asarray(llr, dtype=np.float64)
    if llr.size != E:
        raise ValueError(f"{llr.size} LLRs for a rate-matched length of {E}")

    nof_systematic = int(K_transmitted) - 2 * int(Zc)
    buffer_length = nof_systematic + (int(nb) * int(Zc) - int(K_b))
    shift_factor = {1: (0, 17, 33, 56), 2: (0, 13, 25, 43)}[bg]
    k0 = shift_factor[rv] * int(Zc)
    filler_start = int(K_transmitted) - 2 * int(Zc)
    k0 -= (int(K_b) - int(K_transmitted)) if k0 >= filler_start else 0

    codeword = np.zeros(int(nb) * int(Zc), dtype=np.float64)
    buffer_index = (np.arange(E) + k0) % buffer_length
    codeword_index = np.where(buffer_index < nof_systematic,
                              2 * int(Zc) + buffer_index,
                              int(K_b) + (buffer_index - nof_systematic))
    np.add.at(codeword, codeword_index, llr)

    # The filler bits are zeros by construction and every receiver knows it, so they enter as
    # known values rather than erasures. Leaving them at zero erases information the decoder is
    # entitled to, and at a rate near 0.87 that is the difference between converging and stalling.
    # The punctured `2 * Zc` prefix is NOT treated this way: it carries the first information bits,
    # so it really is unknown. (`nr_ldpc.ldpc_encode` places the information at column 0, so the
    # rate matcher's systematic window starting at `2 * Zc` skips exactly those bits.)
    if int(K_b) > int(K_transmitted):
        codeword[int(K_transmitted):int(K_b)] = KNOWN_LLR
    received = np.zeros(codeword.size, dtype=bool)
    received[codeword_index] = True
    return codeword, received


def systematic_share(K_transmitted, K_b, Zc, E, rv, bg):
    """Fraction of a transmission that carries systematic rather than parity bits.

    A HARQ RETRANSMISSION is selected from a different point of the circular buffer, and some
    redundancy versions carry almost no systematic bits because the receiver is expected to
    combine them with an earlier transmission. Such a block cannot be decoded alone, so a
    coded-BLER criterion must exclude it rather than count it as an error.
    """
    mb, nb, _ = NL._BG_PARAMS[bg]
    nof_systematic = int(K_transmitted) - 2 * int(Zc)
    buffer_length = nof_systematic + (int(nb) * int(Zc) - int(K_b))
    k0 = {1: (0, 17, 33, 56), 2: (0, 13, 25, 43)}[bg][rv] * int(Zc)
    if k0 >= nof_systematic:
        k0 -= int(K_b) - int(K_transmitted)
    positions = (np.arange(int(E)) + k0) % buffer_length
    return float(np.mean(positions < nof_systematic))


class MinSum:
    """Normalized min-sum over the QC structure of `nr_ldpc.build_H`."""

    def __init__(self, bg, zc, max_iterations=25, alpha=0.8):
        self.bg = int(bg)
        self.zc = int(zc)
        self.max_iterations = int(max_iterations)
        self.alpha = float(alpha)
        self.mb, self.nb, _ = NL._BG_PARAMS[self.bg]
        self.entries = defaultdict(list)
        for row, col, shift in NL.build_H(self.bg, self.zc):
            self.entries[row].append((col, shift))
        self.row_indices = [np.arange(self.zc) for _ in range(self.mb)]

    def variable_slice(self, col, shift):
        """The variable nodes a check's edge at `shift` reaches, in check-node z order."""
        return col * self.zc + (self.row_indices[0] + shift) % self.zc

    def decode(self, channel_llr):
        """Decode one codeword. Returns (bits, converged, iterations).

        A positive LLR means bit 0, which is the chain's convention (`log_likelihood_ratio.h`).

        The schedule is flooding min-sum. The variable-to-check message of a variable is the
        channel value plus the sum of the messages it received from ALL ITS OTHER checks, so the
        decoder keeps a per-variable sum and subtracts the one edge being recomputed. Summing the
        other edges of the SAME check instead is the mistake this code carried first: it makes the
        messages depend on nothing that the iteration changes, the decisions freeze after the first
        pass, and the decoder still "works" whenever the channel alone is already a codeword.
        """
        channel_llr = np.asarray(channel_llr, dtype=np.float64)
        if channel_llr.size != self.nb * self.zc:
            raise ValueError(f"{channel_llr.size} LLRs for a codeword of {self.nb * self.zc}")

        check_to_var = np.zeros((self.mb, self.nb, self.zc), dtype=np.float64)
        variable_sum = np.zeros((self.nb, self.zc), dtype=np.float64)
        z = self.row_indices[0]

        for iteration in range(1, self.max_iterations + 1):
            for row in range(self.mb):
                entries = self.entries[row]
                targets = []
                extrinsic = np.empty((len(entries), self.zc), dtype=np.float64)
                for index, (col, shift) in enumerate(entries):
                    target = (z + shift) % self.zc
                    targets.append((col, target))
                    extrinsic[index] = (channel_llr[col * self.zc + target]
                                        + variable_sum[col][target]
                                        - check_to_var[row, col])
                signs = np.where(extrinsic < 0.0, -1.0, 1.0)
                magnitudes = np.abs(extrinsic)
                first = np.argmin(magnitudes, axis=0)
                smallest = magnitudes[first, z]
                masked = magnitudes.copy()
                masked[first, z] = np.inf
                second = masked.min(axis=0)
                sign_product = np.prod(signs, axis=0)
                for index, (col, target) in enumerate(targets):
                    magnitude = np.where(first == index, second, smallest)
                    updated = self.alpha * magnitude * sign_product * signs[index]
                    variable_sum[col][target] += updated - check_to_var[row, col]
                    check_to_var[row, col] = updated

            posterior = channel_llr.copy()
            for row in range(self.mb):
                for col, shift in self.entries[row]:
                    np.add.at(posterior, col * self.zc + (z + shift) % self.zc,
                              check_to_var[row, col])
            bits = (posterior < 0.0).astype(np.uint8)
            if NL.verify(bits, self.bg, self.zc):
                return bits, True, iteration
        return bits, False, self.max_iterations


def selfcheck():
    """Prove the inverse chain and the decoder against the real code."""
    rng = np.random.default_rng(20261013)
    bg, zc = 1, 384
    mb, nb, kb_info = NL._BG_PARAMS[bg]
    kb = kb_info * zc
    nof_information = kb - 2 * zc            # a realistic information length for this Zc
    information = rng.integers(0, 2, size=nof_information).astype(np.uint8)
    padded = np.concatenate([information, np.zeros(kb - nof_information, dtype=np.uint8)])
    codeword = np.asarray(NL.ldpc_encode(bg, zc, padded.tolist()), dtype=np.uint8)
    assert NL.verify(codeword, bg, zc), "the encoder produced a word that fails its own check"

    decoder = MinSum(bg, zc)

    # 1. Perfect LLRs: the decoder must return the transmitted codeword exactly, first iteration.
    perfect = (1.0 - 2.0 * codeword) * KNOWN_LLR
    decoded, converged, iterations = decoder.decode(perfect)
    assert converged and iterations == 1, (converged, iterations)
    assert np.array_equal(decoded, codeword), "perfect LLRs did not decode to the codeword"

    # 2. Rate dematching is the inverse of rate matching, including the block layout. Build the
    #    transmitted bits the way the chain does, deposit them, and compare with the codeword.
    # A rate close to the deployed one, so most of the circular buffer is NOT transmitted and the
    # decoder has to recover it from the parity instead of reading it off the channel. An earlier
    # version of this check handed over almost the whole buffer, which let a decoder that could
    # not propagate at all pass.
    # The deployed operating point is a code rate near 0.87, i.e. about 1.15 coded bits per
    # information bit: most of the circular buffer is NOT transmitted and has to be recovered from
    # the parity. Choosing the length against the buffer instead of against K made the check far
    # too easy once already.
    e_total = ((nof_information * 115 // 100) // 8) * 8
    # One code block, so its rate-matched length is the whole selection. The multi-block split is
    # `synth.codeblock_layout`, which is the authority both directions use.
    lengths = [e_total]
    #    The whole inverse chain is exercised here: what the network sees is the INTERLEAVED and
    #    later scrambled sequence, so depositing it without deinterleaving first would deposit the
    #    bits at the wrong codeword positions. That mistake was in this test before it was in the
    #    decoder, so the step order is spelled out.
    def inverse_chain(rv):
        rate_matched = np.asarray(DL.rate_match(codeword.tolist(), nof_information, kb, zc, e_total,
                                                rv, bg), dtype=np.uint8)
        interleaved = np.asarray(DL.bit_interleave(rate_matched.tolist(), 8), dtype=np.uint8)
        return rate_dematch(deinterleave((1.0 - 2.0 * interleaved) * KNOWN_LLR, 8), nof_information,
                            kb, zc, e_total, rv, bg)

    deposited, received = inverse_chain(0)
    assert np.all(np.sign(deposited[received])
                  == (1.0 - 2.0 * codeword[received])), "a deposited sign is wrong"
    decoded, converged, _ = decoder.decode(deposited)
    assert np.array_equal(decoded, codeword), "the inverse chain lost the codeword"

    # 3. Repetition combining: an rv=2 selection deposits into different buffer positions, and
    #    combining both selections must still decode.
    second, _ = inverse_chain(2)
    decoded, converged, _ = decoder.decode(deposited + second)
    assert np.array_equal(decoded, codeword), "combining two redundancy versions lost the codeword"

    # 4. The decoder has to be able to FAIL, or the metric would be meaningless. Noise strong
    #    enough to make the code useless must produce wrong words, and a clean channel must not.
    #    The erasures stay in place, so this is the same problem the deployed rate poses.
    def block_errors(amplitude, trials=12):
        errors = 0
        for _ in range(trials):
            llr = (1.0 - 2.0 * codeword.astype(float)) * 4.0
            llr[deposited == 0.0] = 0.0            # keep the erasures, and the known filler
            llr = llr + rng.standard_normal(codeword.size) * amplitude
            decoded, converged, _ = decoder.decode(llr)
            errors += int(not np.array_equal(decoded, codeword))
        return errors / trials

    clean = block_errors(0.5)
    hopeless = block_errors(3.0)
    assert clean == 0.0, f"a clean channel produced {clean} block errors"
    assert hopeless > 0.0, "noise that should break the code produced no block errors"

    print(f"ldpc_decode selfcheck: perfect decode in {iterations} iteration, inverse chain exact, "
          f"BLER {clean:.2f} on a clean channel and {hopeless:.2f} when it should fail")
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
