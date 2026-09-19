// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu/phy/upper/equalization/channel_equalizer_device_grid.h"
#include "ocudu/support/ocudu_assert.h"

using namespace ocudu;

void ch_gather_desc::build(const crb_bitmap&                         rb_mask,
                           unsigned                                  first_symbol,
                           unsigned                                  nof_symbols_,
                           const bounded_bitset<MAX_NSYMB_PER_SLOT>& dmrs_symb_pos,
                           uint16_t                                  active_re_per_prb,
                           uint16_t                                  active_re_per_prb_dmrs)
{
  ocudu_assert(nof_ports != 0, "A device gather needs at least one receive port.");
  ocudu_assert(nof_ports <= ch_gather_max_ports, "Too many receive ports for a device gather.");
  ocudu_assert(nof_symbols_ <= ch_gather_max_symbols, "Too many OFDM symbols for a device gather.");
  ocudu_assert(rb_mask.count() <= ch_gather_max_prbs, "Too many PRB for a device gather.");
  nof_symbols = nof_symbols_;
  nof_entries = 0;

  // The geometry the entries are expanded from, kept for the DEVICE builder (batch 5e): it rebuilds
  // the same tables from these, and the unit test compares the two implementations byte for byte.
  geometry = geometry_t{};
  geometry.first_symbol            = first_symbol;
  geometry.active_re_per_prb       = active_re_per_prb;
  geometry.active_re_per_prb_dmrs  = active_re_per_prb_dmrs;
  geometry.dmrs_sym_bits           = 0;
  for (unsigned sym = 0; sym != MAX_NSYMB_PER_SLOT; ++sym) {
    geometry.dmrs_sym_bits |= (dmrs_symb_pos.test(sym) ? 1U : 0U) << sym;
  }
  {
    const uint64_t* words = rb_mask.data();
    const unsigned  nof   = (rb_mask.size() + 63) / 64;
    for (unsigned w = 0; (w != nof) && (w != ch_gather_max_rb_words); ++w) {
      geometry.rb_words[w] = words[w];
    }
  }

  // The plan reproduces the demodulator's RE mask element by element: for every OFDM symbol of the
  // hop, the allocation's PRBs in ascending order and, within each of them, the active subcarriers
  // in ascending order. That is exactly the order in which the resource grid's mask reader
  // (resource_grid_reader_impl::get()) walks the same mask, so the gathered bytes land in the same
  // places the host gather would have put them.
  for (unsigned i_symbol = 0; i_symbol != nof_symbols; ++i_symbol) {
    const unsigned symbol = first_symbol + i_symbol;
    // A data-only symbol leaves the whole PRB to the data; a DM-RS symbol carries data only in the
    // subcarriers the DM-RS pattern (and the CDM groups without data) leaves free.
    const uint16_t active = dmrs_symb_pos.test(symbol) ? active_re_per_prb_dmrs : active_re_per_prb;

    ch_gather_symbol& run = symbols[i_symbol];
    run.symbol            = symbol;
    run.entry_base        = nof_entries;
    run.nof_entries       = 0;

    unsigned dest = 0;
    for (unsigned prb = rb_mask.find_lowest(), last_prb = rb_mask.find_highest(); prb <= last_prb; ++prb) {
      if (!rb_mask.test(prb)) {
        continue;
      }
      const unsigned subc_base = prb * NOF_SUBCARRIERS_PER_RB;
      for (unsigned i_subc = 0; i_subc != NOF_SUBCARRIERS_PER_RB; ++i_subc) {
        if ((active & (1U << i_subc)) == 0) {
          continue;
        }
        ocudu_assert(nof_entries != ch_gather_max_entries, "The gather plan exceeds its entry table.");
        entries[nof_entries] = ch_gather_entry{static_cast<uint16_t>(subc_base + i_subc), static_cast<uint16_t>(dest)};
        ++nof_entries;
        ++dest;
      }
    }
    run.nof_entries = dest;
  }
}
