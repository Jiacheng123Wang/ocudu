// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/phy/lower/processors/uplink/uplink_processor_baseband.h"

namespace ocudu {

class baseband_gateway_buffer_reader;
struct lower_phy_rx_symbol_context;

/// \brief Lower physical layer PUxCH processor - Baseband interface.
///
/// Processes baseband samples with OFDM symbol granularity. The OFDM symbol size is inferred from the slot numerology.
class puxch_processor_baseband
{
public:
  /// Default destructor.
  virtual ~puxch_processor_baseband() = default;

  /// \brief Number of OFDM symbol buffers the caller has to provide.
  ///
  /// The processor keeps transforms of up to this many symbols in flight, each reading the buffer its
  /// symbol was assembled in. The caller owns that many independent buffers and assembles every symbol
  /// in the one acquire_symbol_buffer() hands out.
  virtual unsigned get_nof_symbol_buffers() const = 0;

  /// \brief Acquires the buffer the caller shall assemble the next OFDM symbol into.
  ///
  /// The returned buffer is not read by any transform in flight: when the pipeline is full this call
  /// finishes (and reports) the oldest symbol until the buffer is free. The caller must call it once
  /// per OFDM symbol, before writing the samples, and then pass the index to process_symbol().
  virtual unsigned acquire_symbol_buffer() = 0;

  /// \brief Processes a baseband OFDM symbol.
  ///
  /// \param[in] samples      Baseband samples to process.
  /// \param[in] context      OFDM Symbol context.
  /// \param[in] buffer_index Symbol buffer the samples were assembled in, as returned by
  ///                         acquire_symbol_buffer().
  /// \param[in] owner        Handle keeping the samples alive until the transforms submitted here are
  ///                         finished (see uplink_processor_baseband::rx_buffer_handle), or null when
  ///                         the caller owns the samples for long enough.
  /// \return \c true if the signal is processed, \c false otherwise.
  virtual bool process_symbol(const baseband_gateway_buffer_reader& samples,
                              const lower_phy_rx_symbol_context&    context,
                              unsigned                              buffer_index,
                              uplink_processor_baseband::rx_buffer_handle owner) = 0;
};

} // namespace ocudu
