// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/adt/span.h"
#include "ocudu/phy/upper/equalization/channel_equalizer.h"
#include "ocudu/support/ocudu_assert.h"
#include <vector>

namespace ocudu {

/// \brief Channel estimates list that references storage owned by someone else.
///
/// One read-only span per (Rx port, Tx layer), for consumers that read the estimates where they
/// were produced - e.g. out of the estimator's device (GPU) buffer - instead of gathering them into
/// their own storage. The referenced storage must stay valid and unchanged while the list is in use.
class view_ch_est_list : public channel_equalizer::ch_est_list
{
public:
  view_ch_est_list() = default;

  /// \brief Sets the dimensions of the list. Every (port, layer) entry must be set afterwards.
  void reset(unsigned nof_re_, unsigned nof_rx_ports_, unsigned nof_tx_layers_)
  {
    nof_re      = nof_re_;
    nof_layers  = nof_tx_layers_;
    channels.assign(static_cast<std::size_t>(nof_rx_ports_) * nof_tx_layers_, span<const cbf16_t>());
  }

  /// Sets the estimates of one (Rx port, Tx layer) pair.
  void set_channel(unsigned i_rx_port, unsigned i_layer, span<const cbf16_t> ch)
  {
    ocudu_assert((i_rx_port * nof_layers + i_layer) < channels.size(), "Channel index out of range.");
    ocudu_assert(ch.size() == nof_re, "Invalid number of channel estimates ({} != {}).", ch.size(), nof_re);
    channels[i_rx_port * nof_layers + i_layer] = ch;
  }

  // See interface for documentation.
  span<const cbf16_t> get_channel(unsigned i_rx_port, unsigned i_layer) const override
  {
    ocudu_assert((i_rx_port * nof_layers + i_layer) < channels.size(), "Channel index out of range.");
    return channels[i_rx_port * nof_layers + i_layer];
  }

  // See interface for documentation.
  unsigned get_nof_re() const override { return nof_re; }

  // See interface for documentation.
  unsigned get_nof_rx_ports() const override
  {
    return (nof_layers == 0) ? 0 : static_cast<unsigned>(channels.size()) / nof_layers;
  }

  // See interface for documentation.
  unsigned get_nof_tx_layers() const override { return nof_layers; }

private:
  /// Estimates, indexed by <tt>i_rx_port * nof_layers + i_layer</tt>.
  std::vector<span<const cbf16_t>> channels;
  /// Number of resource elements per channel.
  unsigned nof_re = 0;
  /// Number of transmission layers.
  unsigned nof_layers = 0;
};

} // namespace ocudu
