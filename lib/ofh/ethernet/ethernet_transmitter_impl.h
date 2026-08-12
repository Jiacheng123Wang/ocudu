// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ethernet_tx_metrics_collector_impl.h"
#include "ocudu/ocudulog/logger.h"
#include "ocudu/ofh/ethernet/ethernet_transmitter.h"
#include "ocudu/ofh/ethernet/ethernet_transmitter_config.h"
#if defined(__APPLE__)
#include <net/if.h>
#include <net/if_dl.h>
using sockaddr_ll = struct sockaddr_dl;
#else
#include <linux/if_packet.h>
#endif

namespace ocudu {
namespace ether {

/// Implementation for the Ethernet transmitter.
class transmitter_impl : public transmitter
{
public:
  transmitter_impl(const transmitter_config& config, ocudulog::basic_logger& logger_);
  ~transmitter_impl() override;

  // See interface for documentation.
  void send(span<span<const uint8_t>> frames) override;

  // See interface for documentation.
  transmitter_metrics_collector* get_metrics_collector() override;

private:
#ifdef __linux__
  ocudulog::basic_logger&            logger;
  int                                socket_fd = -1;
  sockaddr_ll                      socket_address;
#endif
  transmitter_metrics_collector_impl metrics_collector;
};

} // namespace ether
} // namespace ocudu
