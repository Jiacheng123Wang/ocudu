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
    device_bases.assign(channels.size(), nullptr);
  }

  /// Sets the estimates of one (Rx port, Tx layer) pair.
  void set_channel(unsigned i_rx_port, unsigned i_layer, span<const cbf16_t> ch)
  {
    set_channel(i_rx_port, i_layer, ch, nullptr);
  }

  /// \brief Sets the estimates of one (Rx port, Tx layer) pair, which live in \c device_base.
  ///
  /// \param[in] device_base Buffer the coefficients were produced in, or nullptr when they live in
  ///                        host memory. The buffer must be the one the producing stage wrapped, so
  ///                        that a backend binding it binds the same device resource (see
  ///                        ch_est_list::device_slice).
  void set_channel(unsigned i_rx_port, unsigned i_layer, span<const cbf16_t> ch, const void* device_base)
  {
    ocudu_assert((i_rx_port * nof_layers + i_layer) < channels.size(), "Channel index out of range.");
    ocudu_assert(ch.size() == nof_re, "Invalid number of channel estimates ({} != {}).", ch.size(), nof_re);
    ocudu_assert((device_base == nullptr) || (ch.data() >= static_cast<const cbf16_t*>(device_base)),
                 "The channel estimates start before the given device buffer.");
    channels[i_rx_port * nof_layers + i_layer]     = ch;
    device_bases[i_rx_port * nof_layers + i_layer] = device_base;
  }

  // See interface for documentation.
  span<const cbf16_t> get_channel(unsigned i_rx_port, unsigned i_layer) const override
  {
    ocudu_assert((i_rx_port * nof_layers + i_layer) < channels.size(), "Channel index out of range.");
    return channels[i_rx_port * nof_layers + i_layer];
  }

  // See interface for documentation.
  std::optional<device_slice> get_device_slice(unsigned i_rx_port) const override
  {
    if ((i_rx_port * nof_layers + nof_layers) > channels.size()) {
      return std::nullopt;
    }
    const std::size_t base_index = static_cast<std::size_t>(i_rx_port) * nof_layers;
    const void*       base       = device_bases[base_index];
    if ((base == nullptr) || (nof_layers == 0)) {
      return std::nullopt;
    }

    device_slice slice;
    slice.base         = base;
    slice.offset       = static_cast<std::size_t>(channels[base_index].data() - static_cast<const cbf16_t*>(base));
    slice.nof_layers   = nof_layers;
    slice.layer_stride = (nof_layers > 1) ? static_cast<unsigned>(channels[base_index + 1].data() -
                                                                  channels[base_index].data())
                                          : 0;
    // Every layer must sit in the same buffer, at the same distance from the previous one: the
    // slice describes the whole port with one base and one stride.
    for (unsigned i_layer = 1; i_layer != nof_layers; ++i_layer) {
      if (device_bases[base_index + i_layer] != base) {
        return std::nullopt;
      }
      if (channels[base_index + i_layer].data() - channels[base_index + i_layer - 1].data() !=
          static_cast<ptrdiff_t>(slice.layer_stride)) {
        return std::nullopt;
      }
    }
    return slice;
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
  /// Buffer each entry was produced in, or nullptr when it lives in host memory (same indexing).
  std::vector<const void*> device_bases;
  /// Number of resource elements per channel.
  unsigned nof_re = 0;
  /// Number of transmission layers.
  unsigned nof_layers = 0;
};

} // namespace ocudu
