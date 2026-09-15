// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "../../../../../gateways/baseband/baseband_gateway_buffer_test_doubles.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/phy/lower/lower_phy_rx_symbol_context.h"
#include "ocudu/phy/lower/processors/lower_phy_center_freq_controller.h"
#include "ocudu/phy/lower/processors/uplink/puxch/puxch_processor_baseband.h"
#include "ocudu/phy/lower/processors/uplink/puxch/puxch_processor_factories.h"
#include "ocudu/phy/lower/processors/uplink/puxch/puxch_processor_notifier.h"
#include "ocudu/phy/lower/processors/uplink/puxch/puxch_processor_request_handler.h"
#include "ocudu/phy/support/resource_grid_context.h"
#include "ocudu/phy/support/shared_resource_grid.h"
#include <vector>

namespace ocudu {

class puxch_processor_baseband_spy : public puxch_processor_baseband
{
public:
  struct entry_t {
    baseband_gateway_buffer_read_only samples;
    lower_phy_rx_symbol_context       context;
    unsigned                          buffer_index = 0;
  };

  unsigned get_nof_symbol_buffers() const override { return nof_symbol_buffers; }

  unsigned acquire_symbol_buffer() override
  {
    unsigned buffer = next_buffer;
    next_buffer     = (next_buffer + 1) % nof_symbol_buffers;
    return buffer;
  }

  bool process_symbol(const baseband_gateway_buffer_reader& samples,
                      const lower_phy_rx_symbol_context&    context,
                      unsigned                              buffer_index) override
  {
    entries.emplace_back();
    entry_t& entry      = entries.back();
    entry.samples       = samples;
    entry.context       = context;
    entry.buffer_index  = buffer_index;
    return true;
  }

  const std::vector<entry_t>& get_entries() const { return entries; }

  void clear() { entries.clear(); }

  /// Number of symbol buffers handed to the processor, mirroring the real one.
  static constexpr unsigned nof_symbol_buffers = 8;

private:
  std::vector<entry_t> entries;
  unsigned             next_buffer = 0;
};

class puxch_processor_request_handler_spy : public puxch_processor_request_handler
{
public:
  struct entry_t {
    const resource_grid*  grid;
    resource_grid_context context;
  };

  void handle_request(const shared_resource_grid& grid, const resource_grid_context& context) override
  {
    entries.emplace_back();
    entry_t& entry = entries.back();
    entry.grid     = &grid.get();
    entry.context  = context;
  }

  const std::vector<entry_t>& get_entries() const { return entries; }

private:
  std::vector<entry_t> entries;
};

class puxch_processor_spy : public puxch_processor, private lower_phy_center_freq_controller
{
public:
  puxch_processor_spy(const puxch_processor_configuration& config_) : config(config_) {}

  void connect(puxch_processor_notifier& notifier_) override { notifier = &notifier_; }

  void stop() override {}

  puxch_processor_request_handler& get_request_handler() override { return request_handler; }

  puxch_processor_baseband& get_baseband() override { return baseband; }

  lower_phy_center_freq_controller& get_center_freq_control() override { return *this; }

  const puxch_processor_configuration& get_configuration() const { return config; }

  const puxch_processor_notifier* get_notifier() const { return notifier; }

  const std::vector<puxch_processor_baseband_spy::entry_t>& get_baseband_entries() const
  {
    return baseband.get_entries();
  }

  void clear() { baseband.clear(); }

private:
  bool set_carrier_center_frequency(double carrier_center_frequency_Hz) override { return false; }

  puxch_processor_notifier*           notifier = nullptr;
  puxch_processor_configuration       config;
  puxch_processor_request_handler_spy request_handler;
  puxch_processor_baseband_spy        baseband;
};

class puxch_processor_factory_spy : public puxch_processor_factory
{
public:
  std::unique_ptr<puxch_processor> create(const puxch_processor_configuration& config) override
  {
    std::unique_ptr<puxch_processor_spy> ptr = std::make_unique<puxch_processor_spy>(config);
    instance                                 = ptr.get();
    return std::move(ptr);
  }

  puxch_processor_spy& get_spy() { return *instance; }

private:
  puxch_processor_spy* instance = nullptr;
};

} // namespace ocudu
