#!/usr/bin/env python3
"""Uplink control information multiplexed on the PUSCH shared channel, as the chain lays it out.

The shared channel does not occupy every resource element of a data symbol once the reception
carries control information. `get_ulsch_information` (`lib/ran/pusch/ulsch_info.cpp`) turns the
payload sizes into a number of resource elements per control part, and
`ulsch_demultiplex_impl::configure_current_ofdm_symbol` (`lib/phy/upper/channel_processors/pusch/`)
decides WHICH elements, walking the data symbols in ascending order and spreading each part over
the elements that are still free. This module reproduces both, so that the offline re-encoder maps
the shared channel onto the same elements the air interface did.

What changes, and what does not
-------------------------------
* CSI Part 1 and HARQ-ACK of more than two bits are REMOVED from the shared channel: the rate
  matching length is `(N_RE - Q') * Qm` and the codeword has proportionally fewer bits.
* HARQ-ACK of one or two bits does NOT change the length: the standard keeps G and punctures the
  reserved elements, so the shared channel's bits are still generated for them and the transmitted
  symbol there is the control placeholder instead. Those elements are marked `punctured`: they
  consume codeword bits but do not carry them.

The scrambling index is the element's position in the FULL allocation
--------------------------------------------------------------------
`pusch_demodulator_impl` descrambles each OFDM symbol over all of its allocated elements, UCI
included, with one continuous sequence across the codeword (`pusch_demodulator_impl.cpp`, the
per-symbol loop that calls `descrambler->generate`). The descrambling bit of a shared-channel
element is therefore indexed by its position in the allocation traversal, not by its index in the
shared-channel sequence. The two coincide when there is no control information, which is why
contiguous scrambling was right for every reception that carried none.

CSI Part 2 is refused, not approximated. Its payload size is a function of the decoded CSI Part 1
(`uci_part2_get_size`), and the capture records only the size description's entry count, so the
number of bits it takes is not recoverable from the sidecar. A unit that reports it raises rather
than being treated as if it were absent.
"""

import math

import numpy as np

import nr_ldpc

#: Resource elements a DM-RS symbol spends per PRB for each CDM group without data (TS 38.211,
#: Section 6.4.1.1.2): type 1 uses every other subcarrier, type 2 every third.
NOF_RE_PER_PRB = {1: 6, 2: 4}

#: Lifting sizes of TS 38.212, Table 5.3.2-1, ascending. `compute_lifting_size` scans this order.
LIFTING_SIZES = tuple(sorted(z for row in nr_ldpc.LIFTING_SIZE_TABLE for z in row))


class UciLayout:
    """The control payload as the demultiplexer is configured: payload bits and element counts.

    This is the smallest description the label path needs, and it is exactly what the receiver's
    demultiplexer takes: `nof_harq_ack_bits`, `nof_enc_harq_ack_bits`, `nof_harq_ack_rvd` and
    `nof_enc_csi_part1_bits`. Keeping the payload size AND the element counts means a consumer does
    not have to know the transport block's segmentation to reproduce the map, which is what lets the
    dataset store it in four small arrays instead of re-deriving it per sample.
    """

    __slots__ = ("nof_harq_ack", "nof_ack_re", "nof_ack_rvd_re", "nof_csi1_re")

    def __init__(self, nof_harq_ack=0, nof_ack_re=0, nof_ack_rvd_re=0, nof_csi1_re=0):
        self.nof_harq_ack = int(nof_harq_ack)
        self.nof_ack_re = int(nof_ack_re)
        self.nof_ack_rvd_re = int(nof_ack_rvd_re)
        self.nof_csi1_re = int(nof_csi1_re)

    def _values(self):
        return (self.nof_harq_ack, self.nof_ack_re, self.nof_ack_rvd_re, self.nof_csi1_re)

    def __eq__(self, other):
        return isinstance(other, UciLayout) and self._values() == other._values()

    def __hash__(self):
        return hash(self._values())

    def __repr__(self):
        return (f"UciLayout(harq_ack={self.nof_harq_ack}, ack_re={self.nof_ack_re}, "
                f"ack_rvd_re={self.nof_ack_rvd_re}, csi1_re={self.nof_csi1_re})")

    @property
    def is_empty(self):
        """Whether the control payload leaves the shared channel's element map untouched."""
        return not (self.nof_ack_re or self.nof_ack_rvd_re or self.nof_csi1_re)


EMPTY = UciLayout()


class ReSets:
    """Per-symbol resource-element sets `configure_current_ofdm_symbol` ends up with.

    Each field is a list over the OFDM symbols of the transmission; each entry is a boolean array
    over the subcarriers of the allocation. A DM-RS symbol has no element, because the builder
    models the two CDM groups without data that cover it.
    """

    __slots__ = ("ulsch", "ack", "csi1", "rvd")

    def __init__(self, ulsch, ack, csi1, rvd):
        self.ulsch = ulsch
        self.ack = ack
        self.csi1 = csi1
        self.rvd = rvd


class DataLayout:
    """Where the shared channel's codeword lands, given the control payload.

    * `e_total` is the codeword length: the elements that consume a codeword group times `Qm`, so
      `(N_RE - Q') * Qm` for the parts that are removed and the full `N_RE * Qm` when the only part
      present is a one-or-two-bit HARQ-ACK;
    * `ulsch_pos` is every resource element the codeword consumes, in the chain's order: symbol
      major, then ascending subcarrier. It INCLUDES the elements HARQ-ACK of one or two bits
      punctures, because the codeword is rate-matched over them.
    * `data_pos` is the subset that actually carries a shared-channel symbol, i.e. `ulsch_pos` less
      the punctured elements. This is what a re-encoder may compare against a capture.
    * `scramble_re` is, for every entry of `ulsch_pos`, the element's position in the full
      allocation traversal, which is the index the descrambling sequence is read at.
    * `keep` is a boolean mask over the `Qm`-bit groups of the codeword: false where the group is
      punctured.
    """

    __slots__ = ("e_total", "ulsch_pos", "data_pos", "scramble_re", "keep", "nof_full_re", "qm",
                 "re_sets", "nof_re_total", "nof_re_ul_sch")

    def __init__(self, e_total, ulsch_pos, data_pos, scramble_re, keep, nof_full_re, qm, re_sets,
                 nof_re_total, nof_re_ul_sch):
        self.e_total = int(e_total)
        self.ulsch_pos = ulsch_pos
        self.data_pos = data_pos
        self.scramble_re = scramble_re
        self.keep = keep
        self.nof_full_re = int(nof_full_re)
        self.qm = int(qm)
        self.re_sets = re_sets
        self.nof_re_total = int(nof_re_total)
        self.nof_re_ul_sch = int(nof_re_ul_sch)

    @property
    def nof_punctured(self):
        """Number of shared-channel elements HARQ-ACK of one or two bits occupies instead."""
        return int(self.keep.size - self.keep.sum())

    @property
    def keep_bits(self):
        """Boolean mask over the codeword's bits, false for the bits of a punctured element."""
        return np.repeat(self.keep, self.qm)


def uci_crc_bits(nof_payload_bits):
    """CRC bits of one control payload (`get_uci_crc_size`, TS 38.212 Section 6.3.1.2.1)."""
    if nof_payload_bits < 12:
        return 0
    if nof_payload_bits < 20:
        return 6
    return 11


def sch_segmentation(tb_bits, target_code_rate):
    """The shared channel's segmentation as `get_sch_segmentation_info` derives it.

    Returns `(base_graph, nof_cb, nof_bits_per_cb)`. `nof_cb * nof_bits_per_cb` is the denominator
    of the three control element formulas, so it has to come from the CHAIN's model of the
    transport block rather than from the encoder's own lifting-size search: the two agree on a
    single code block and the chain's is the one the element counts were computed with.

    `target_code_rate` is R times 1024, the form the capture sidecar records (the PDU's
    `mcs_descr.target_code_rate`).
    """
    # A payload size, a bit list or a bit array: the element formulas only need its length.
    tbs = int(tb_bits) if np.ndim(tb_bits) == 0 else int(np.size(tb_bits))
    if tbs <= 0:
        raise ValueError("a shared channel with no payload has no control element formulas")
    tb_crc = 16 if tbs <= 3824 else 24
    # get_ldpc_base_graph: the SCHEDULED rate, not the realized one.
    rate = float(target_code_rate) / 1024.0
    if not 0.0 < rate < 1.0:
        raise ValueError(f"target code rate {target_code_rate}/1024 is outside (0, 1)")
    if tbs <= 292 or rate <= 0.25 or (tbs <= 3824 and rate <= 0.67):
        base_graph = 2
    else:
        base_graph = 1
    payload = tbs + tb_crc
    max_segment_length = 8448 if base_graph == 1 else 3840
    if payload <= max_segment_length:
        nof_cb = 1
    else:
        nof_cb = -(-payload // (max_segment_length - 24))
    # compute_lifting_size: BG1 counts 22 information columns, BG2 6/8/9/10 by payload size, and
    # the total spans every code block.
    if base_graph == 1:
        ref_length = 22
    elif payload > 640:
        ref_length = 10
    elif payload > 560:
        ref_length = 9
    elif payload > 192:
        ref_length = 8
    else:
        ref_length = 6
    total = payload + (24 * nof_cb if nof_cb > 1 else 0)
    lifting_size = None
    for candidate in LIFTING_SIZES:
        if candidate * nof_cb * ref_length >= total:
            lifting_size = candidate
            break
    if lifting_size is None:
        raise ValueError(f"no lifting size fits {total} bits in {nof_cb} code blocks")
    nof_bits_per_cb = (22 if base_graph == 1 else 10) * lifting_size
    return base_graph, nof_cb, nof_bits_per_cb


def _ceil_ratio(nof_bits, beta_offset, nof_re_uci, sum_nof_cb_size):
    """`ceil((O + L) * beta * N_RE / sum(K))` in the chain's single-precision arithmetic.

    `ulsch_info.cpp` casts each factor to `float` and multiplies left to right. The distinction
    from double precision is not pedantic: a product that lands on an exact integer would round up
    under one precision and not the other, and the element count would differ by one.
    """
    value = np.float32(np.float32(nof_bits) * np.float32(beta_offset))
    value = np.float32(value * np.float32(nof_re_uci))
    value = np.float32(value / np.float32(sum_nof_cb_size))
    return int(math.ceil(float(value)))


def nof_ack_re(nof_harq_ack, beta_offset_harq_ack, nof_re_uci, sum_nof_cb_size, alpha_scaling,
               nof_re_uci_l0):
    """`calculate_nof_re_harq_ack`: Q'_ACK in resource elements."""
    if nof_harq_ack == 0:
        return 0
    left = _ceil_ratio(nof_harq_ack + uci_crc_bits(nof_harq_ack), beta_offset_harq_ack,
                       nof_re_uci, sum_nof_cb_size)
    right = int(math.ceil(float(np.float32(np.float32(alpha_scaling)
                                           * np.float32(nof_re_uci_l0)))))
    return min(left, right)


def nof_csi1_re(nof_csi_part1, beta_offset_csi_part1, nof_re_uci, nof_ack_re_for_csi1,
                sum_nof_cb_size, alpha_scaling):
    """`calculate_nof_re_csi_part1`: Q'_CSI1 in resource elements."""
    if nof_csi_part1 == 0:
        return 0
    left = _ceil_ratio(nof_csi_part1 + uci_crc_bits(nof_csi_part1), beta_offset_csi_part1,
                       nof_re_uci, sum_nof_cb_size)
    right = int(math.ceil(float(np.float32(np.float32(alpha_scaling)
                                           * np.float32(nof_re_uci)))))
    # The chain subtracts the HARQ-ACK reservation from an UNSIGNED value, so a negative result
    # wraps and the `min` selects `left`. Computing the wrap explicitly would make the outcome
    # depend on the width of the type; the outcome is `left`, and that is what is returned.
    right -= nof_ack_re_for_csi1
    if right < 0:
        return left
    return min(left, right)


def nof_re_uci_counts(nof_prb, nof_symbols, dmrs_symbols, dmrs_type=1,
                      nof_cdm_groups_without_data=2):
    """The three element counts `get_ulsch_information` forms before the control formulas.

    Returns `(nof_re_total, nof_re_uci, nof_re_uci_l0)`: the elements available to the shared
    channel, the elements that can carry control information (data symbols only), and the ones
    after the first DM-RS symbol, which is where the demultiplexer may place HARQ-ACK.
    """
    dmrs = sorted(int(s) for s in dmrs_symbols)
    nof_re_per_prb = NOF_RE_PER_PRB[int(dmrs_type)]
    nof_re_dmrs_per_rb = len(dmrs) * int(nof_cdm_groups_without_data) * nof_re_per_prb
    nof_re_total = int(nof_prb) * (int(nof_symbols) * 12 - nof_re_dmrs_per_rb)
    nof_re_uci = (int(nof_symbols) - len(dmrs)) * int(nof_prb) * 12
    if not dmrs:
        raise ValueError("a transmission without DM-RS symbols has no shared channel")
    nof_re_uci_l0 = 0
    for symbol in range(dmrs[0], int(nof_symbols)):
        if symbol not in dmrs:
            nof_re_uci_l0 += int(nof_prb) * 12
    return nof_re_total, nof_re_uci, nof_re_uci_l0


def from_capture(cfg, tb_bits, nof_prb, dmrs_symbols, dmrs_type, nof_cdm_groups_without_data):
    """Read a capture sidecar's control description and turn it into a `UciLayout`.

    The sidecar records the payload sizes and the beta offsets (`ul_capture.cpp`), so the element
    counts have to be computed here exactly as the scheduler's own `get_ulsch_information` did.
    A reception whose control payload leaves the map untouched returns `EMPTY`, which is what keeps
    the control-free majority on the original code path.

    CSI Part 2 raises: the sidecar carries the size description's entry COUNT and the payload size
    is a function of the decoded Part 1, so a bit count cannot be recovered from it.
    """
    if int(cfg.get("nof_csi_part2_entries", 0) or 0) != 0:
        raise ValueError("the capture multiplexes CSI part 2, whose payload size is not in the "
                         "sidecar: refusing rather than laying the shared channel out without it")
    nof_harq_ack = int(cfg.get("nof_harq_ack", 0) or 0)
    nof_csi_part1 = int(cfg.get("nof_csi_part1", 0) or 0)
    if nof_harq_ack == 0 and nof_csi_part1 == 0:
        return EMPTY
    alpha_scaling = float(cfg.get("alpha_scaling", 0.0) or 0.0)
    beta_offset_harq_ack = float(cfg.get("beta_offset_harq_ack", 0.0) or 0.0)
    beta_offset_csi_part1 = float(cfg.get("beta_offset_csi_part1", 0.0) or 0.0)
    nof_symbols = int(cfg.get("nof_symbols", 14))
    _, nof_cb, nof_bits_per_cb = sch_segmentation(tb_bits, cfg["target_code_rate"])
    sum_nof_cb_size = nof_cb * nof_bits_per_cb
    _, nof_re_uci, nof_re_uci_l0 = nof_re_uci_counts(
        nof_prb, nof_symbols, dmrs_symbols, dmrs_type, nof_cdm_groups_without_data)

    ack_re = nof_ack_re(nof_harq_ack, beta_offset_harq_ack, nof_re_uci, sum_nof_cb_size,
                        alpha_scaling, nof_re_uci_l0)
    if nof_harq_ack < 2:
        # The reservation is computed as if the payload were two bits (ulsch_info.cpp, the
        # `nof_harq_ack_bits < 2` branch). It is what the reserved element set holds, and CSI Part 1
        # is placed around it even when the actual ACK occupies fewer elements than that.
        ack_rvd_re = nof_ack_re(2, beta_offset_harq_ack, nof_re_uci, sum_nof_cb_size,
                                alpha_scaling, nof_re_uci_l0)
    elif nof_harq_ack == 2:
        ack_rvd_re = ack_re
    else:
        ack_rvd_re = 0
    ack_re_for_csi1 = ack_rvd_re if nof_harq_ack <= 2 else ack_re
    csi1_re = nof_csi1_re(nof_csi_part1, beta_offset_csi_part1, nof_re_uci, ack_re_for_csi1,
                          sum_nof_cb_size, alpha_scaling)
    return UciLayout(nof_harq_ack, ack_re, ack_rvd_re, csi1_re)


def _select(re_set, d, m_re_count):
    """`re_set_select`: the first `m_re_count` set elements, keeping every `d`-th one."""
    out = np.zeros(re_set.size, dtype=bool)
    positions = np.flatnonzero(re_set)
    if positions.size < m_re_count:
        raise AssertionError(f"cannot select {m_re_count} elements from {positions.size}")
    out[positions[np.arange(0, m_re_count * d, d)]] = True
    return out


def _nof_per_symbol(nof_prb, nof_symbols, dmrs_symbols, dmrs_type, nof_cdm_groups_without_data,
                    with_control):
    """`M_ulsch` of one symbol: the whole allocation, or nothing on a DM-RS symbol.

    A DM-RS symbol that still carries data elements (fewer CDM groups without data than the DM-RS
    type allows) is outside what the builder models. It only raises when there IS control
    information to place: without any, the element map is the one the builder has always used, and
    refusing those units would remove samples to no benefit.
    """
    nof_re_per_prb = NOF_RE_PER_PRB[int(dmrs_type)]
    data_re_per_prb = 12 - int(nof_cdm_groups_without_data) * nof_re_per_prb
    if with_control and data_re_per_prb != 0:
        raise ValueError(
            f"{nof_cdm_groups_without_data} CDM groups of DM-RS type {dmrs_type} leave "
            f"{data_re_per_prb} data elements per PRB on a DM-RS symbol, and the control payload "
            f"is multiplexed around them: the builder cannot model that allocation")
    dmrs = set(int(s) for s in dmrs_symbols)
    return [nof_prb * 12 if symbol not in dmrs else 0 for symbol in range(nof_symbols)]


def re_sets(nof_prb, nof_symbols, dmrs_symbols, uci=None, dmrs_type=1,
            nof_cdm_groups_without_data=2):
    """Reproduce `configure_current_ofdm_symbol` over the transmission, symbol by symbol.

    The order in which the parts are placed matters and is the chain's: reserve for HARQ-ACK, place
    HARQ-ACK if it has more than two bits, place CSI Part 1, then place the one-or-two-bit HARQ-ACK
    on the reserved set. Each part takes the next free elements, striding by `d = free // needed`
    when it does not need every element, and the strides are recomputed after each part, so the
    placement of CSI Part 1 depends on what HARQ-ACK already took.

    Every quantity the chain works with is a resource element count scaled by `Qm * layers` and then
    divided by it again, so the MAP does not depend on the modulation or on the number of layers.
    """
    nof_symbols = int(nof_symbols)
    dmrs = set(int(s) for s in dmrs_symbols)
    if not dmrs:
        raise ValueError("a transmission without DM-RS symbols has no shared channel")
    if min(dmrs) < 0 or max(dmrs) >= nof_symbols:
        raise ValueError(f"DM-RS symbols {sorted(dmrs)} are outside 0..{nof_symbols - 1}")
    uci = uci if uci is not None else EMPTY
    nof_per_symbol = _nof_per_symbol(nof_prb, nof_symbols, dmrs_symbols, dmrs_type,
                                     nof_cdm_groups_without_data, with_control=not uci.is_empty)
    # get_ulsch_demultiplex_l1 and _l1_csi: HARQ-ACK starts after the first DM-RS symbol, CSI Part 1
    # at the first data symbol.
    first_dmrs = min(dmrs)
    l1 = next(symbol for symbol in range(first_dmrs, nof_symbols) if symbol not in dmrs)
    l1_csi = next(symbol for symbol in range(nof_symbols) if symbol not in dmrs)
    # The encoded lengths are the element counts times `Qm * layers`, and every use divides by the
    # same factor, so the counts themselves are what the map is built from.
    nof_enc_ack = uci.nof_ack_re
    nof_enc_ack_rvd = uci.nof_ack_rvd_re
    nof_enc_csi1 = uci.nof_csi1_re

    ulsch_sets, ack_sets, csi1_sets, rvd_sets = [], [], [], []
    m_rvd_count = m_ack_count = m_csi1_count = 0
    for symbol in range(nof_symbols):
        nof_re = nof_per_symbol[symbol]
        ulsch = np.ones(nof_re, dtype=bool)
        # UCI can only be placed where DM-RS is not transmitted.
        uci_set = (np.ones(nof_re, dtype=bool) if symbol not in dmrs
                   else np.zeros(nof_re, dtype=bool))
        rvd = np.zeros(nof_re, dtype=bool)
        ack = np.zeros(nof_re, dtype=bool)
        csi1 = np.zeros(nof_re, dtype=bool)
        if nof_re == 0:
            ulsch_sets.append(ulsch)
            ack_sets.append(ack)
            csi1_sets.append(csi1)
            rvd_sets.append(rvd)
            continue
        m_uci = int(uci_set.sum())

        # Step 1. Reserve potential elements for HARQ-ACK.
        remainder = nof_enc_ack_rvd - m_rvd_count
        if symbol >= l1 and m_uci > 0 and remainder > 0:
            if remainder < m_uci:
                d, m = m_uci // remainder, remainder
            else:
                d, m = 1, m_uci
            rvd = _select(ulsch, d, m)
            m_rvd_count += m

        # Step 2. Place HARQ-ACK when it carries more than two bits. Those elements leave the
        # shared channel, so the codeword is shorter.
        remainder = nof_enc_ack - m_ack_count
        if symbol >= l1 and m_uci > 0 and uci.nof_harq_ack > 2 and remainder > 0:
            if remainder < m_uci:
                d, m = m_uci // remainder, remainder
            else:
                d, m = 1, m_uci
            ack = _select(uci_set, d, m)
            ulsch &= ~ack
            uci_set &= ~ack
            m_uci = int(uci_set.sum())
            m_ack_count += m

        # Step 3. Place CSI Part 1 around the reserved elements.
        remainder = nof_enc_csi1 - m_csi1_count
        m_rvd = int(rvd.sum())
        if symbol >= l1_csi and (m_uci - m_rvd) > 0 and remainder > 0:
            available = m_uci - m_rvd
            if remainder < available:
                d, m = available // remainder, remainder
            else:
                d, m = 1, available
            candidate = ~rvd & uci_set
            csi1 = _select(candidate, d, m)
            ulsch &= ~csi1
            uci_set &= ~csi1
            m_csi1_count += m

        # Step 5. Place HARQ-ACK of one or two bits on the reserved elements. The elements stay in
        # the shared channel's set: their codeword bits are transmitted over by the placeholder.
        remainder = nof_enc_ack - m_ack_count
        if m_rvd > 0 and uci.nof_harq_ack <= 2 and remainder > 0:
            if remainder < m_rvd:
                d, m = m_rvd // remainder, remainder
            else:
                d, m = 1, m_rvd
            ack = _select(rvd, d, m)
            m_ack_count += m

        ulsch_sets.append(ulsch)
        ack_sets.append(ack)
        csi1_sets.append(csi1)
        rvd_sets.append(rvd)
    # The chain places every control element it was configured with; the scheduler's element count
    # is what fits. A shortfall means this reproduction disagrees with the chain, which must be an
    # error rather than a map that looks plausible.
    for name, placed, configured in (("HARQ-ACK", m_ack_count, nof_enc_ack),
                                     ("reserved", m_rvd_count, nof_enc_ack_rvd),
                                     ("CSI part 1", m_csi1_count, nof_enc_csi1)):
        if placed != configured:
            raise AssertionError(f"placed {placed} {name} elements, the chain describes "
                                 f"{configured}")
    return ReSets(ulsch_sets, ack_sets, csi1_sets, rvd_sets)


def data_positions(nof_prb, nof_symbols, dmrs_symbols, uci=None, dmrs_type=1,
                   nof_cdm_groups_without_data=2, sets=None):
    """The shared channel's element map and descrambling indices, without the codeword length.

    `nof_symbols` counts the symbols of the transmission from symbol 0; the caller's grid has to be
    addressed the same way. The builder's grids are 14 symbols from symbol 0, which the capture
    sidecars confirm for this corpus.

    Returns `(ulsch_pos, data_pos, scramble_re, keep, nof_full_re)`; see `DataLayout` for what each
    one means.
    """
    nof_symbols = int(nof_symbols)
    dmrs = sorted(int(s) for s in dmrs_symbols)
    if sets is None:
        sets = re_sets(nof_prb, nof_symbols, dmrs, uci, dmrs_type, nof_cdm_groups_without_data)
    nof_prb = int(nof_prb)
    ulsch_pos, data_pos, scramble_re, keep = [], [], [], []
    full_index = 0
    for symbol in range(nof_symbols):
        if symbol in dmrs:
            continue
        ulsch = sets.ulsch[symbol]
        ack = sets.ack[symbol]
        for subcarrier in range(nof_prb * 12):
            if ulsch[subcarrier]:
                ulsch_pos.append((symbol, subcarrier))
                scramble_re.append(full_index)
                # The placeholder of a one-or-two-bit HARQ-ACK overwrites the symbol here; the
                # codeword bit is consumed but not transmitted.
                transmitted = not ack[subcarrier]
                keep.append(transmitted)
                if transmitted:
                    data_pos.append((symbol, subcarrier))
            full_index += 1
    return ulsch_pos, data_pos, scramble_re, np.asarray(keep, dtype=bool), full_index


def data_layout(nof_prb, nof_symbols, dmrs_symbols, qm, uci=None, nof_layers=1, dmrs_type=1,
                nof_cdm_groups_without_data=2):
    """The shared channel's element map and its codeword length.

    `E = nof_ul_sch_re * Qm * layers`, where `nof_ul_sch_re` is the number of elements that consume
    a codeword group: the whole allocation less the parts that are removed.
    """
    if int(nof_layers) != 1:
        raise ValueError(f"the builder re-encodes a single layer, not {nof_layers}")
    dmrs = sorted(int(s) for s in dmrs_symbols)
    sets = re_sets(nof_prb, nof_symbols, dmrs, uci, dmrs_type, nof_cdm_groups_without_data)
    ulsch_pos, data_pos, scramble_re, keep, nof_full_re = data_positions(
        nof_prb, nof_symbols, dmrs, uci, dmrs_type, nof_cdm_groups_without_data, sets=sets)
    nof_re_total, _, _ = nof_re_uci_counts(nof_prb, nof_symbols, dmrs, dmrs_type,
                                           nof_cdm_groups_without_data)
    nof_re_ul_sch = len(ulsch_pos)
    return DataLayout(nof_re_ul_sch * int(qm) * int(nof_layers), ulsch_pos, data_pos, scramble_re,
                      keep, nof_full_re, int(qm), sets, nof_re_total, nof_re_ul_sch)


def descrambling_sequence(layout, c_init):
    """The scrambling bits of the transmitted shared-channel symbols, in codeword order.

    The sequence is generated over the whole allocation and read at each element's position in it,
    which is what the chain's per-symbol descrambler does. With no control information the
    positions are the codeword's own indices and this is `DD.scramble`'s sequence.
    """
    import dd_label as DL

    gold = np.asarray(DL.gold_seq(int(c_init), layout.nof_full_re * layout.qm), dtype=np.uint8)
    index = (np.asarray(layout.scramble_re, dtype=np.int64)[:, None] * layout.qm
             + np.arange(layout.qm)[None, :])
    return gold[index].reshape(-1)[layout.keep_bits]


def scramble(coded_bits, layout, c_init):
    """Scramble the codeword at its elements' positions in the full allocation.

    `coded_bits` is the whole codeword, in the chain's order, including the bits HARQ-ACK of one or
    two bits punctures. Returns the scrambled bits that are actually transmitted, in the order of
    `layout.data_pos`.
    """
    coded = np.asarray(coded_bits, dtype=np.uint8)
    if coded.size != layout.e_total:
        raise ValueError(f"{coded.size} coded bits for a codeword of {layout.e_total}")
    sequence = descrambling_sequence(layout, c_init)
    return coded[layout.keep_bits] ^ sequence
