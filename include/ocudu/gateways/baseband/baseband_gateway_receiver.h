// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/gateways/baseband/baseband_gateway_base.h"
#include "ocudu/gateways/baseband/baseband_gateway_timestamp.h"

namespace ocudu {

class baseband_gateway_buffer_writer;

/// Baseband gateway - reception interface.
class baseband_gateway_receiver : public baseband_gateway_base
{
public:
  /// \brief What the radio itself reported about a received block (dev doc 6.51).
  ///
  /// The radio knows things the host cannot infer from a timestamp, and until this field existed they were
  /// thrown away: UHD classifies every receive as late / overflow / broken-chain, but the classification only
  /// produced a warning line, while the PHY's continuity check counted a gap without knowing why. Measured on
  /// the fused-lane air legs, the two are the same event - across eleven legs the UHD `overflow` count equalled
  /// the continuity checker's gap count, event for event: an overflow is the radio's receive ring filling up and
  /// dropping samples because the host (or the USB transfer) did not drain it in time. Carrying the code here
  /// lets the receive path report the radio's own verdict next to its gap count instead of leaving the reader to
  /// reconstruct it from timestamp arithmetic.
  enum class rx_error {
    /// The radio reported no error for this block (also what radios without a classification leave).
    none = 0,
    /// The block was delivered with a late command, i.e. the request was issued after its samples were due.
    late,
    /// The radio's receive ring filled and the samples this block should have carried were DROPPED by the
    /// radio - the discontinuity a host cannot repair (see the gap counting in the lower PHY).
    overflow,
    /// Any other error the radio reported (broken chain, alignment, bad packet).
    other
  };

  /// Receiver metadata.
  struct metadata {
    /// Timestamp of the received baseband signal.
    baseband_gateway_timestamp ts;
    /// What the radio reported about this block (see rx_error).
    rx_error error = rx_error::none;
  };

  /// \brief Receives a number of baseband samples.
  /// \param[out,in] data Buffer of baseband samples.
  /// \return Receiver metadata.
  /// \note The \c data buffer provides the number of samples to receive through \ref
  ///       baseband_gateway_buffer::get_nof_samples.
  /// \note The \c data buffer must have the same number of channels as the stream.
  virtual metadata receive(baseband_gateway_buffer_writer& data) = 0;
};

} // namespace ocudu
