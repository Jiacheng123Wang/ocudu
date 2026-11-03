#!/usr/bin/env python3
"""Coded block error rate of the depth-3 main head, through the real LDPC code.

It lives beside the trainer rather than inside it because it is the decoder's side of the contract,
not the training loop's: the same code is what a validation pass, a gate or an analysis script needs,
and it has to be importable without TensorFlow so the contract tests can pin it (`depth3/test`).
"""
import depth3_paths
depth3_paths.add_ce_shared()
import numpy as np

import dd_label as DL              # noqa: E402
import depth3 as D3                # noqa: E402
import ldpc_decode as LD           # noqa: E402
import pusch_uci as UCI            # noqa: E402
import synth as SN                 # noqa: E402

NOF_SYMBOLS = D3.NOF_SYMBOLS


class CodedBler:
    """Coded block error rate of the main head, through the real LDPC code.

    Uncoded BER says the LLRs point the right way; only decoding says whether the code can still
    correct what they got wrong, and `design` section 2.3 makes coded BLER the criterion. Each
    sample is decoded block by block: the LLRs are descrambled, deinterleaved and deposited into
    the codeblock's circular buffer, then decoded, and a block counts as an error unless
    re-encoding the decoded word reproduces the transmitted bits exactly. That last step is what a
    receiver's codeblock CRC decides, and it needs no reference codeword.

    The decoders are cached per (base graph, lifting size): building the QC structures costs more
    than decoding one block.
    """

    def __init__(self, batches, max_iterations=25):
        self.batches = batches
        self.decoders = {}
        self.max_iterations = max_iterations
        self.blocks = 0
        self.errors = 0
        self.skipped = 0

    def decoder(self, bg, zc):
        key = (int(bg), int(zc))
        if key not in self.decoders:
            self.decoders[key] = LD.MinSum(bg, zc, max_iterations=self.max_iterations)
        return self.decoders[key]

    def add(self, index, llr_chain):
        """Decode one sample's LLRs, in the chain's codeword order."""
        data = self.batches.data
        alloc = self.batches.allocation_of(index)
        dmrs_symbols = self.batches.dmrs_symbols_of(index)
        modulation = self.batches.modulation_of(index)
        qm = D3.BITS_PER_SYMBOL[modulation]
        nof_tb = int(data["nof_tb_bits"][index])
        tb_bits = np.unpackbits(data["tb_bits"][index])[:nof_tb]
        transmitted = np.unpackbits(data["bits"][index])[:int(data["nof_bits"][index])]
        if transmitted.size != llr_chain.size:
            raise ValueError(f"{llr_chain.size} LLRs for {transmitted.size} transmitted bits")

        dmrs_mask = sum(1 << s for s in dmrs_symbols)
        uci = self.batches.uci_of(index)
        code_blocks, bg, zc, kb, e_total, lengths = SN.codeblock_layout(
            tb_bits, len(alloc), NOF_SYMBOLS, qm, dmrs_mask, uci)
        rv = int(data["rv"][index])
        # The scrambling bits of the TRANSMITTED symbols, read at each element's position in the
        # allocation. With no control information those positions are the codeword's own indices
        # and this is the contiguous sequence the old code generated.
        layout = UCI.data_layout(len(alloc), NOF_SYMBOLS, dmrs_symbols, qm, uci)
        sequence = UCI.descrambling_sequence(layout, (int(data["rnti"][index]) << 15)
                                             + int(data["n_id"][index]))
        assert sequence.size == transmitted.size, (sequence.size, transmitted.size)
        # THE PUNCTURED ELEMENTS ARE ERASURES IN PLACE, NOT ABSENT (2026-11-03). A HARQ-ACK of one
        # or two bits does not shorten the codeword: `ulsch_info.cpp` counts the reserved elements
        # in `nof_ul_sch_bits` (only an ACK of more than two bits is subtracted), and the
        # demultiplexer zeroes the soft bits of the placeholder elements (`ocuduvec::zero`) instead
        # of dropping them. The codeword is therefore `e_total` long with a run of exactly-zero LLRs
        # where the control payload sits. Decoding the carried bits as if they were the whole
        # codeword -- which is what this function did until now -- hands the rate dematcher a
        # shorter vector than its own block layout and raises on every such sample.
        carried = layout.keep_bits
        if carried.size != e_total or carried.sum() != transmitted.size:
            raise ValueError(f"{carried.sum()} carried bits, {transmitted.size} transmitted, "
                             f"{e_total} in the codeword")
        descrambled = np.zeros(e_total, dtype=np.float64)
        descrambled[carried] = llr_chain * (1.0 - 2.0 * sequence.astype(np.float64))
        # The scrambling sequence covers the whole codeword, so the comparison below stays in the
        # descrambled domain. Re-scrambling one block on its own would restart the sequence at
        # position 0 and mismatch everything except the first block, which is exactly how this
        # looked before it was fixed: every block decoded exactly and five of six "failed".
        descrambled_bits = np.zeros(e_total, dtype=np.uint8)
        descrambled_bits[carried] = transmitted ^ sequence
        decoder = self.decoder(bg, zc)

        offset = 0
        for block, e_cb in enumerate(lengths):
            block_llr = descrambled[offset:offset + e_cb]
            block_bits = descrambled_bits[offset:offset + e_cb]
            block_carried = carried[offset:offset + e_cb]
            offset += e_cb
            # A retransmission that carries almost no systematic bits cannot be decoded on its own:
            # the receiver is expected to combine it with an earlier transmission first. Counting
            # it as a block error would report the HARQ design as a network failure.
            #
            # ONLY RETRANSMISSIONS (design section 19.563). The share is also below the threshold at
            # rv = 0 for the smallest BG2 blocks -- at the minimum BG2 size the systematic bits are
            # eight of fifty-two columns, a share of 0.154 -- and those decode perfectly well on
            # their own. Applying the rule there skipped 170 of 5742 first transmissions in the
            # acceptance set, all of them BG2 with transport blocks under 2800 bits, which is exactly
            # the small-block regime the gates are supposed to judge.
            if rv != 0 and LD.systematic_share(len(code_blocks[block]), kb, zc, e_cb, rv, bg) < 0.2:
                self.skipped += 1
                continue
            buffer, _ = LD.rate_dematch(LD.deinterleave(block_llr, qm), len(code_blocks[block]), kb,
                                        zc, e_cb, rv, bg)
            decoded, converged, _ = decoder.decode(buffer)
            # Re-encoding the decoded word and comparing with what was transmitted is the same
            # decision a codeblock CRC makes, and it needs no reference codeword.
            rematched = np.asarray(DL.rate_match(list(decoded), len(code_blocks[block]), kb, zc,
                                                 e_cb, rv, bg), dtype=np.uint8)
            rematched = np.asarray(DL.bit_interleave(rematched.tolist(), qm), dtype=np.uint8)
            self.blocks += 1
            # ONLY THE BITS THAT WERE ON AIR DECIDE THE BLOCK. The punctured positions were never
            # transmitted, so a disagreement there is the erasure the decoder was told to expect,
            # not a decoding failure -- counting it would fail every block that carries a one- or
            # two-bit HARQ-ACK.
            self.errors += int(not np.array_equal(rematched[block_carried],
                                                  block_bits[block_carried]))
        assert offset == e_total, (offset, e_total)

    def rate(self):
        """Block error rate over the blocks that are decodable alone, and how many were not."""
        if not self.blocks:
            return float("nan")
        return self.errors / self.blocks

    def summary(self):
        return f"{self.rate():.4f} over {self.blocks} blocks ({self.skipped} skipped as HARQ)"
