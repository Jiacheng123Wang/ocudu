// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/adt/complex.h"
#include "ocudu/adt/span.h"
#include "ocudu/gateways/baseband/baseband_gateway_timestamp.h"
#include "ocudu/gateways/baseband/buffer/baseband_gateway_buffer_dynamic.h"
#include <memory>

namespace ocudu {

class baseband_gateway_buffer_reader;

/// \brief Lower physical layer uplink processor - Baseband interface.
///
/// Processes baseband samples. It derives the symbol and slot timing from the number of processed samples.
class uplink_processor_baseband
{
public:
  /// Default destructor.
  virtual ~uplink_processor_baseband() = default;

  /// \brief Handle that keeps a receive buffer alive while a transform reads it.
  ///
  /// The radio fills a buffer and hands it over; the uplink processor starts transforms that read it
  /// asynchronously, so the buffer cannot go back to the radio's pool when process() returns. The
  /// caller passes the buffer's handle, the processor keeps a reference per in-flight transform, and
  /// the buffer returns to the pool when its last reference is dropped - i.e. when the transform
  /// reading it has been finished (see ofdm_symbol_demodulator::finish_symbol()).
  ///
  /// A null handle means the caller guarantees the lifetime itself (a test, or a caller whose samples
  /// outlive every transform it asks for); nothing is then kept alive on its behalf.
  using rx_buffer_handle = std::shared_ptr<baseband_gateway_buffer_dynamic_aligned>;

  /// \brief Processes any number of baseband samples.
  ///
  /// \param[in] buffer    Baseband samples to process.
  /// \param[in] timestamp Time instant in which the first sample was captured.
  /// \param[in] owner     Handle keeping \c buffer alive until its transforms are finished, or null.
  /// \remark The number of channels in \c buffer must be equal to the number of receive ports for the sector.
  virtual void
  process(const baseband_gateway_buffer_reader& buffer, baseband_gateway_timestamp timestamp, rx_buffer_handle owner) = 0;
};

} // namespace ocudu
