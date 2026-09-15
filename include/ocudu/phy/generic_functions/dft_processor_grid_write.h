// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/adt/complex.h"
#include "ocudu/adt/span.h"
#include "ocudu/phy/support/resource_grid_device_view.h"

namespace ocudu {

/// \brief One OFDM symbol of one port written into the resource grid by a DFT engine.
///
/// A DFT engine able to write the grid reproduces the host post-processing of the transform output exactly
/// (ocudu::ocuduvec::sc_prod with the per-symbol coefficient, then ocudu::ocuduvec::prod with the window table, then
/// the conversion to the grid's cbf16 storage), including the upper/lower band mapping: grid subcarrier \c i takes
/// transform element \c (i + map_offset) % size.
struct dft_grid_write_params {
  /// Device view of the grid storage the symbol is written into.
  resource_grid_device_view view = {};
  /// Port the symbol belongs to.
  unsigned port = 0;
  /// OFDM symbol within the slot.
  unsigned symbol = 0;
  /// Number of subcarriers of the grid to write.
  unsigned nof_subc = 0;
  /// Rotation applied to the transform output (see the struct comment): \c size - nof_subc / 2 for the resource grid.
  unsigned map_offset = 0;
  /// Per-symbol compensation applied to every element: phase compensation times the output scaling.
  cf_t coefficient = 1.0F;
  /// Apply the per-element table published with set_grid_write_window() (the DFT window phase compensation).
  bool apply_window = false;
};

/// \brief Optional DFT capability: write the demodulated symbol into the resource grid.
///
/// The grid write is encoded in the same command buffer as the transform it consumes, so a device-side grid costs one
/// dispatch, not one command buffer (the CPU would otherwise pay a round trip per symbol for it). The grid storage is
/// the one the CPU reads afterwards (see resource_grid_device_view), so the caller that needs the grid on the host
/// only has to make sure the engine's work completed.
///
/// The engine never decides *whether* the chain wants the grid written from the device: the caller asks for it. A
/// transform whose grid write is not requested (or not possible) is submitted through the plain entry point, and the
/// caller post-processes it on the host, exactly as before this capability existed.
class dft_processor_grid_write
{
public:
  /// Default destructor.
  virtual ~dft_processor_grid_write() = default;

  /// Whether this engine can write \c view (the view must be valid and belong to a grid of this transform's size).
  virtual bool supports_grid_write(const resource_grid_device_view& view) const = 0;

  /// \brief Publishes the per-element compensation table (one complex entry per transform element).
  ///
  /// The table is the DFT window phase compensation the host post-processing applies before the band mapping. It is
  /// constant for the lifetime of the demodulator, so it is uploaded once.
  ///
  /// \param[in] window Table, or an empty span to clear it (then no write applies a window).
  /// \return True on success.
  virtual bool set_grid_write_window(span<const cf_t> window) = 0;

  /// \brief Submits the transform held in slot \c slot together with the write of one grid symbol, without waiting.
  ///
  /// Replaces the plain asynchronous submission for this symbol: the caller has already filled the transform input of
  /// the slot (dft_processor::get_input()) and must not submit it again through the plain entry point.
  ///
  /// \param[in] slot   Slot of the transform (as in dft_processor::run_async()).
  /// \param[in] params Where and how the symbol is written.
  /// \return True when the dispatch was encoded and committed.
  virtual bool submit_grid_write(unsigned slot, const dft_grid_write_params& params) = 0;
};

} // namespace ocudu
