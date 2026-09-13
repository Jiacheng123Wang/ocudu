// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/adt/complex.h"
#include "ocudu/adt/span.h"
#include "ocudu/support/macos_compat.h"
#include <cstdint>

namespace ocudu {

/// \brief Device view of a resource grid buffer.
///
/// The grid storage is a page-aligned host allocation (see page_aligned_allocator) that a Metal engine wraps as a
/// buffer with shared storage, so a GPU writer fills the grid in place and the CPU sees the very same memory once the
/// device work of the slot completed. This descriptor is all a device writer needs to address one element of the grid
/// - no host-side layout knowledge, no copy.
///
/// \note "Device" here means "addressable by the accelerator", not "a different memory": with unified memory the base
/// is the grid's own buffer, so there is nothing to publish back to the host afterwards.
struct resource_grid_device_view {
  /// Storage base, page-aligned and covering whole pages. Null when the grid is not device-addressable.
  void* base = nullptr;
  /// Elements between two consecutive subcarriers.
  unsigned subc_stride = 1;
  /// Elements between two consecutive OFDM symbols.
  unsigned symb_stride = 0;
  /// Elements between two consecutive ports.
  unsigned port_stride = 0;
  /// Number of subcarriers of the view.
  unsigned nof_subc = 0;
  /// Number of OFDM symbols of the view.
  unsigned nof_symb = 0;
  /// Number of ports of the view.
  unsigned nof_ports = 0;

  /// Whether the grid storage can be addressed by a device writer.
  bool is_valid() const { return base != nullptr; }

  /// \brief Byte offset of an OFDM symbol of a port, i.e. the start of the run of \c nof_subc subcarriers a device
  /// writer fills at once.
  unsigned get_symbol_offset(unsigned port, unsigned symbol) const
  {
    return port * port_stride + symbol * symb_stride;
  }
};

/// \brief Builds the device view of a grid storage buffer.
///
/// The layout is the one of the grid tensor: subcarriers contiguous within an OFDM symbol, symbols within a port and
/// ports last.
///
/// \param[in] storage    Whole storage of the grid.
/// \param[in] nof_subc   Number of subcarriers of the grid.
/// \param[in] nof_symb   Number of OFDM symbols of the grid.
/// \param[in] nof_ports  Number of ports of the grid.
/// \return The device view, invalid when \c storage is empty or its base is not page-aligned (a device engine cannot
/// map it without a copy, so its writers keep running on the host).
inline resource_grid_device_view make_resource_grid_device_view(span<cbf16_t> storage,
                                                               unsigned       nof_subc,
                                                               unsigned       nof_symb,
                                                               unsigned       nof_ports)
{
  resource_grid_device_view view;
  if (storage.empty()) {
    return view;
  }

  const std::size_t page = compat::page_size();
  if ((reinterpret_cast<uintptr_t>(storage.data()) % page) != 0) {
    return view;
  }

  view.base        = storage.data();
  view.subc_stride = 1;
  view.symb_stride = nof_subc;
  view.port_stride = nof_subc * nof_symb;
  view.nof_subc    = nof_subc;
  view.nof_symb    = nof_symb;
  view.nof_ports   = nof_ports;
  return view;
}

} // namespace ocudu
