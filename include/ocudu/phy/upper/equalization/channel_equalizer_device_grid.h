// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Where the equalizer's received symbols come from when they are read off the device grid.
///
/// The PUSCH demodulator hands the equalizer the received symbols of one OFDM symbol as a
/// re_buffer_reader over the resource grid. On the host that reader is a view of (or a copy of) the
/// grid storage; on the device it cannot be expressed, because the resource elements the equalizer
/// consumes are not a contiguous run of the grid - the DM-RS comb and the per-PRB active pattern
/// leave holes - and because one grid holds several ports. This plan says, in the coordinates a
/// device kernel indexes the grid with, exactly which resource elements of one OFDM symbol the
/// equalizer consumes and where they go in its input staging buffer, so the gather runs on the GPU
/// instead of one memcpy per port and symbol on the host.
///
/// The gather is a transport: every element is a cbf16_t copied bit for bit, never converted, so a
/// device gather and a host gather produce the same bytes by construction. The plan therefore
/// carries no arithmetic - only layout.

#pragma once

#include "ocudu/adt/bounded_bitset.h"
#include "ocudu/adt/complex.h"
#include "ocudu/adt/span.h"
#include "ocudu/phy/support/resource_grid_device_view.h"
#include "ocudu/ran/cyclic_prefix.h"
#include "ocudu/ran/resource_allocation/rb_bitmap.h"
#include "ocudu/ran/resource_block.h"
#include <array>
#include <cstdint>

namespace ocudu {

/// Maximum number of receive ports a gather plan describes.
static constexpr unsigned ch_gather_max_ports = 4;

/// Maximum number of OFDM symbols a gather plan describes.
static constexpr unsigned ch_gather_max_symbols = MAX_NSYMB_PER_SLOT;

/// Maximum number of resource blocks a gather plan describes.
static constexpr unsigned ch_gather_max_prbs = MAX_NOF_PRBS;

/// Maximum number of resource elements one port of a gather plan describes.
static constexpr unsigned ch_gather_max_entries = ch_gather_max_symbols * ch_gather_max_prbs * NOF_SUBCARRIERS_PER_RB;

/// One resource element of a gather: the grid subcarrier it comes from and where it goes.
struct ch_gather_entry {
  /// Grid subcarrier index of the element.
  uint16_t subc = 0;
  /// Destination index of the element within its symbol's output region.
  uint16_t dest = 0;
};

/// \brief What one OFDM symbol of a hop contributes to a gather: its slice of the entry table.
struct ch_gather_symbol {
  /// OFDM symbol index in the resource grid.
  unsigned symbol = 0;
  /// First entry of the symbol within the plan's entry table.
  unsigned entry_base = 0;
  /// Number of entries of the symbol, i.e. its number of resource elements.
  unsigned nof_entries = 0;
};

/// \brief Device gather plan of one hop: which grid elements become the equalizer's input.
///
/// Layout only. It is built from the same RE mask the host gather consumes
/// (pusch_demodulator_impl::get_ch_data_re()), so the two agree element for element: every OFDM
/// symbol of the hop owns a run of consecutive entries which lists, in ascending subcarrier order,
/// the subcarrier of every resource element the demodulator's RE mask selects within the allocation,
/// and \c entries[i].dest is that element's index within the symbol's output region - the position
/// the host gather writes it to.
///
/// The output region of a symbol holds one run of \c nof_re cbf16_t per receive port, in port order,
/// where \c nof_re is the symbol's number of entries - which is why the plan carries no stride: a
/// symbol of the hop that carries DM-RS has fewer entries than a data-only one, and the caller's
/// input buffer (the equalizer's own staging layout) holds the same count per symbol. The gathering
/// kernel writes every port, so the plan's port count is the number of ports read from the grid, not
/// the number the equalizer combines.
///
/// \note The plan is meant to be built ONCE per allocation and handed to every symbol of the hop
///       (the demodulator announces one symbol per submit): the entry table is the expensive part,
///       and it does not depend on the symbol - only the slice a symbol owns does.
class ch_gather_desc
{
public:
  /// \brief An empty plan: no device gather.
  ///
  /// An invalid plan (see is_valid()) is what a caller announces for a symbol it gathered on the
  /// host, and what a backend treats exactly like "read the received symbols the caller passed in".
  ch_gather_desc() = default;

  /// Describes one hop. An invalid \c grid means "no device gather": the caller keeps reading the
  /// grid on the host and must not consult this plan.
  ///
  /// \param[in] grid    Device view of the resource grid.
  /// \param[in] rb_mask Allocation of the hop, in absolute common resource blocks.
  /// \param[in] first_symbol First OFDM symbol of the hop.
  /// \param[in] nof_symbols Number of OFDM symbols of the hop.
  /// \param[in] nof_ports Number of receive ports the caller will read from the grid.
  /// \param[in] dmrs_symb_pos Positions of the DM-RS symbols within the slot.
  /// \param[in] active_re_per_prb Active subcarriers of a data-only PRB, as a 12-bit mask (bit \c n
  ///                   set means subcarrier \c n of the PRB carries data).
  /// \param[in] active_re_per_prb_dmrs Same, for a PRB of a DM-RS symbol.
  ch_gather_desc(const resource_grid_device_view&          grid,
                 const crb_bitmap&                         rb_mask,
                 unsigned                                  first_symbol,
                 unsigned                                  nof_symbols,
                 unsigned                                  nof_ports,
                 const bounded_bitset<MAX_NSYMB_PER_SLOT>& dmrs_symb_pos,
                 uint16_t                                  active_re_per_prb,
                 uint16_t                                  active_re_per_prb_dmrs) :
    grid(grid), nof_ports(nof_ports)
  {
    build(rb_mask, first_symbol, nof_symbols, dmrs_symb_pos, active_re_per_prb, active_re_per_prb_dmrs);
  }

  /// Whether the plan can be gathered on the device.
  bool is_valid() const { return grid.is_valid() && (nof_symbols != 0); }

  /// Number of entries the plan holds (the resource elements of one port of the hop).
  unsigned size() const { return nof_entries; }

  /// Device view of the grid the plan reads.
  resource_grid_device_view grid;
  /// Number of receive ports gathered, i.e. the number of output runs per symbol.
  unsigned nof_ports = 0;
  /// Number of OFDM symbols of the hop.
  unsigned nof_symbols = 0;
  /// What each OFDM symbol of the hop contributes (only the first \ref nof_symbols are meaningful).
  std::array<ch_gather_symbol, ch_gather_max_symbols> symbols{};
  /// The gather entries, grouped by OFDM symbol in hop order. Only the first \ref size() are
  /// meaningful.
  std::array<ch_gather_entry, ch_gather_max_entries> entries{};

private:
  /// Fills \ref symbols, \ref nof_symbols and \ref entries from the demodulator's RE mask.
  void build(const crb_bitmap&                         rb_mask,
             unsigned                                  first_symbol,
             unsigned                                  nof_symbols,
             const bounded_bitset<MAX_NSYMB_PER_SLOT>& dmrs_symb_pos,
             uint16_t                                  active_re_per_prb,
             uint16_t                                  active_re_per_prb_dmrs);

  /// Number of entries the plan holds (the resource elements of one port of the hop).
  unsigned nof_entries = 0;
};

} // namespace ocudu
