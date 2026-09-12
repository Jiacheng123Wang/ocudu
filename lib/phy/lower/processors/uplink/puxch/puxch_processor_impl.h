// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "../../resource_request_pool.h"
#include "ocudu/adt/circular_array.h"
#include "ocudu/phy/lower/lower_phy_rx_symbol_context.h"
#include "ocudu/phy/lower/modulation/ofdm_demodulator.h"
#include "ocudu/phy/lower/processors/lower_phy_center_freq_controller.h"
#include "ocudu/phy/lower/processors/uplink/puxch/puxch_processor.h"

#include <array>
#include "ocudu/phy/lower/processors/uplink/puxch/puxch_processor_baseband.h"
#include "ocudu/phy/lower/processors/uplink/puxch/puxch_processor_notifier.h"
#include "ocudu/phy/lower/processors/uplink/puxch/puxch_processor_request_handler.h"
#include "ocudu/phy/support/shared_resource_grid.h"
#include "ocudu/ran/slot_point.h"

namespace ocudu {

/// Implements PUxCH baseband processor.
class puxch_processor_impl : public puxch_processor,
                             private puxch_processor_baseband,
                             private lower_phy_center_freq_controller,
                             private puxch_processor_request_handler
{
public:
  struct configuration {
    cyclic_prefix cp;
    unsigned      nof_rx_ports;
    unsigned      dft_size;
  };

  puxch_processor_impl(std::unique_ptr<ofdm_symbol_demodulator> demodulator_, const configuration& config) :
    nof_symbols_per_slot(get_nsymb_per_slot(config.cp)),
    nof_rx_ports(config.nof_rx_ports),
    demodulator(std::move(demodulator_))
  {
    ocudu_assert(demodulator, "Invalid demodulator.");
  }

  // See interface for documentation.
  void connect(puxch_processor_notifier& notifier_) override { notifier = &notifier_; }

  // See interface for documentation.
  void stop() override { stopped = true; }

  // See interface for documentation.
  puxch_processor_request_handler& get_request_handler() override { return *this; }

  // See interface for documentation.
  puxch_processor_baseband& get_baseband() override { return *this; }

  // See interface for documentation.
  lower_phy_center_freq_controller& get_center_freq_control() override;

private:
  // See interface for documentation.
  bool process_symbol(const baseband_gateway_buffer_reader& samples,
                      const lower_phy_rx_symbol_context&    context) override;

  // See interface for documentation.
  void handle_request(const shared_resource_grid& grid, const resource_grid_context& context) override;

  // See interface for documentation.
  bool set_carrier_center_frequency(double carrier_center_frequency_Hz) override;

  /// rief One symbol whose DFT is still in flight (submitted but not post-processed yet).
  struct in_flight_symbol {
    lower_phy_rx_symbol_context context;
    unsigned                    slot = 0;
    /// True for the last receive port of an OFDM symbol: the upper PHY is notified once every port
    /// of the symbol has been written into the grid. Notifying on the first port instead would
    /// report a symbol whose remaining ports are still missing from the grid, which the PUCCH -
    /// two symbols long and combining every port - cannot tolerate.
    bool last_port = true;
  };

  /// Maximum number of in-flight transforms (one per port and symbol) tracked by the pipeline.
  static constexpr unsigned max_in_flight_symbols = 16;

  /// rief Waits for the oldest in-flight symbol, writes it into the grid and reports it.
  void finish_oldest_symbol();

  /// rief Finishes every in-flight symbol (used at the end of a slot and on slot changes).
  void drain_pipeline();

  std::atomic<bool>                           stopped = false;
  unsigned                                    nof_symbols_per_slot;
  unsigned                                    nof_rx_ports;
  puxch_processor_notifier*                   notifier = nullptr;
  std::unique_ptr<ofdm_symbol_demodulator>    demodulator;
  slot_point                                  current_slot;
  shared_resource_grid                        current_grid;

  // Pipelined demodulation bookkeeping: a FIFO of the transforms (one per port and symbol) whose DFT
  // was submitted but not yet post-processed. The ring position of each submission advances monotonically, so a slot is
  // reused exactly when its transform is finished (depth submissions later).
  std::array<in_flight_symbol, max_in_flight_symbols> in_flight = {};
  unsigned                                            in_flight_begin  = 0;
  unsigned                                            nof_in_flight    = 0;
  unsigned                                            next_pipeline_slot = 0;
  resource_request_pool<shared_resource_grid> requests;
};

} // namespace ocudu
