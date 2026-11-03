#!/usr/bin/env python3
"""A codeword that carries uplink control decodes, and the wrong element map is refused.

WHY IT EXISTS. The coded-BLER path crashed on the first sample that carried control, and the crash
was the good outcome: the two conventions it had wrong both produce a plausible number instead of an
error once the size check is out of the way. `nof_bits` is the number of coded bits that were ON AIR,
which is smaller than the codeword; and the codeword itself stays `e_total` long, because a one- or
two-bit HARQ-ACK PUNCTURES its reserved elements rather than shortening the code -- the chain keeps
those elements in `nof_ul_sch_bits` (`lib/ran/pusch/ulsch_info.cpp`: only an ACK of more than two
bits is subtracted) and zeroes their soft bits (`ulsch_demultiplex_impl`: `ocuduvec::zero`). A
decoder that treats the carried bits as the whole codeword therefore rate-dem matches a vector that
is shorter than its own block layout, and one that fills the puncture with data fails every block
that carries control.

WHAT IT PINS. Two samples, one from each family: a two-bit HARQ-ACK (punctured reserved elements,
the family that crashed) and a four-bit one (elements removed from the shared channel, which was
already right). Both must decode with PERFECT LLRs to zero block errors; the same reception gathered
over the whole allocation -- the shape the trainer used to produce -- must be refused rather than
decoded. Nothing here needs TensorFlow, a capture or a radio.
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "ai_train"))


class OneReception:
    """The slice of `Batches` that `CodedBler.add` reads, for a single sample."""

    def __init__(self, sample, allocation, dmrs_symbols, modulation, uci):
        self.data = {key: np.asarray([value]) for key, value in sample.items()}
        self.allocation = list(allocation)
        self.dmrs_symbols = list(dmrs_symbols)
        self.modulation = modulation
        self.uci = uci

    def allocation_of(self, index):
        return self.allocation

    def dmrs_symbols_of(self, index):
        return self.dmrs_symbols

    def modulation_of(self, index):
        return self.modulation

    def uci_of(self, index):
        return self.uci


def reception(uci, n_prb=12, modulation="QPSK", dmrs_symbols=(2, 7, 11), nof_tb=1024):
    """Re-encode one transport block and return what the decoder needs, plus the transmitted bits."""
    import depth3 as D3
    import synth as SN
    import pusch_uci as UCI

    qm = D3.BITS_PER_SYMBOL[modulation]
    dmrs_mask = sum(1 << symbol for symbol in dmrs_symbols)
    rng = np.random.default_rng(20261103)
    tb_bits = rng.integers(0, 2, size=nof_tb).astype(np.uint8)
    rnti, n_id = 0x4601, 321
    symbols, data_pos, bits = SN.encode_symbols(tb_bits, n_prb, D3.NOF_SYMBOLS, qm, dmrs_mask,
                                                n_id, rnti, 0, return_bits=True, uci=uci)
    assert symbols.size == len(data_pos) == bits.size // qm, (symbols.size, len(data_pos), bits.size)
    sample = {
        "nof_tb_bits": np.int32(tb_bits.size),
        "tb_bits": np.packbits(tb_bits),
        "bits": np.packbits(bits),
        "nof_bits": np.int32(bits.size),
        "rv": np.uint8(0),
        "rnti": np.uint16(rnti),
        "n_id": np.uint16(n_id),
    }
    return sample, bits, list(range(n_prb))


def main():
    try:
        import coded_bler
        import depth3 as D3
        import pusch_uci as UCI
    except ImportError as error:              # pragma: no cover - reported as a CTest skip
        print(f"SKIP: cannot import the decoder side ({error})")
        return 77

    cases = [
        # One or two HARQ-ACK bits: the reserved elements are punctured and stay in the codeword.
        ("two-bit HARQ-ACK", UCI.UciLayout(nof_harq_ack=2, nof_ack_re=8, nof_ack_rvd_re=16)),
        # More than two: the ACK elements leave the shared channel, so nothing is punctured.
        ("four-bit HARQ-ACK", UCI.UciLayout(nof_harq_ack=4, nof_ack_re=16, nof_ack_rvd_re=0)),
    ]
    summary = []
    for name, uci in cases:
        sample, bits, allocation = reception(uci)
        layout = UCI.data_layout(len(allocation), D3.NOF_SYMBOLS, [2, 7, 11], 2, uci)
        punctured = int(layout.keep.size - layout.keep.sum())
        if name.startswith("two"):
            assert punctured > 0, "the two-bit case must punctuate for this test to mean anything"
        else:
            assert punctured == 0, f"the four-bit case must not punctuate, got {punctured}"

        # PERFECT LLRs in the domain the head emits: a positive LLR is a transmitted zero. With no
        # noise the only way to fail is a geometry that disagrees with the code.
        llr = 100.0 * (1.0 - 2.0 * bits.astype(np.float64))
        bler = coded_bler.CodedBler(OneReception(sample, allocation, [2, 7, 11], "QPSK", uci))
        bler.add(0, llr)
        assert bler.blocks > 0, f"{name}: no block was decoded"
        assert bler.errors == 0, (f"{name}: {bler.errors} of {bler.blocks} blocks failed on perfect "
                                  f"LLRs -- the punctured elements are not where the decoder puts them")

        # THE OLD SHAPE MUST BE REFUSED where the two differ. A plane gathered over the whole
        # allocation carries the punctured elements as if they were data; decoding it would silently
        # misalign every block. At four ACK bits nothing is punctured, so the two are the same vector
        # and there is nothing to refuse -- which is exactly why the defect needed a control-bearing
        # sample to appear at all.
        if punctured:
            whole = np.zeros(layout.e_total, dtype=np.float64)
            try:
                coded_bler.CodedBler(
                    OneReception(sample, allocation, [2, 7, 11], "QPSK", uci)).add(0, whole)
            except ValueError:
                pass
            else:
                raise AssertionError(f"{name}: a full-allocation plane was decoded instead of refused")
        summary.append(f"{name}: {bler.blocks} block(s), {punctured} punctured element(s)")

    print("OK  control-bearing decode: " + "; ".join(summary))
    return 0


if __name__ == "__main__":
    sys.exit(main())
