// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "../../../../gateways/baseband/baseband_gateway_buffer_test_doubles.h"
#include "prach/prach_processor_test_doubles.h"
#include "puxch/puxch_processor_test_doubles.h"
#include "ocudu/phy/lower/lower_phy_baseband_metrics.h"
#include "ocudu/phy/lower/lower_phy_timing_context.h"
#include "ocudu/phy/lower/processors/lower_phy_cfo_controller.h"
#include "ocudu/phy/lower/processors/uplink/uplink_processor.h"
#include "ocudu/phy/lower/processors/uplink/uplink_processor_baseband.h"
#include "ocudu/phy/lower/processors/uplink/uplink_processor_factories.h"
#include "ocudu/phy/lower/processors/uplink/uplink_processor_notifier.h"

namespace ocudu {

class uplink_processor_notifier_spy : public uplink_processor_notifier
{
public:
  void on_half_slot(const lower_phy_timing_context& context) override { half_slots.emplace_back(context); }

  void on_full_slot(const lower_phy_timing_context& context) override { full_slots.emplace_back(context); }

  void on_new_metrics(const lower_phy_baseband_metrics& metrics) override { baseband_metrics.emplace_back(metrics); }

  const std::vector<lower_phy_timing_context>& get_half_slots() const { return half_slots; }

  const std::vector<lower_phy_timing_context>& get_full_slots() const { return full_slots; }

  const std::vector<lower_phy_baseband_metrics>& get_metrics() const { return baseband_metrics; }

  void clear_notifications()
  {
    half_slots.clear();
    full_slots.clear();
    baseband_metrics.clear();
  }

private:
  std::vector<lower_phy_timing_context>  half_slots;
  std::vector<lower_phy_timing_context>  full_slots;
  std::vector<lower_phy_baseband_metrics> baseband_metrics;
};

class uplink_processor_baseband_spy : public uplink_processor_baseband
{
public:
  struct entry_t {
    baseband_gateway_buffer_read_only buffer;
    baseband_gateway_timestamp        timestamp;
    /// Handle the caller passed with the samples (see uplink_processor_baseband::rx_buffer_handle).
    rx_buffer_handle owner;
  };

  void process(const baseband_gateway_buffer_reader& buffer,
               baseband_gateway_timestamp            timestamp,
               rx_buffer_handle                      owner) override
  {
    entries.emplace_back();
    entry_t& entry  = entries.back();
    entry.timestamp = timestamp;
    entry.buffer    = buffer;
    entry.owner     = std::move(owner);
  }

  /// \brief Size in samples of every symbol of the grid this spy describes, or 0 for "no grid".
  ///
  /// The receive side asks the radio for whole symbols only when the uplink processor describes the
  /// grid (see lower_phy_baseband_processor::ul_process), and a uniform grid is enough to exercise that
  /// policy: the real, uneven grid is asserted in the uplink processor's own test.
  void set_symbol_size(unsigned size) { symbol_size = size; }

  // See interface for documentation.
  symbol_grid_position locate_symbols(baseband_gateway_timestamp timestamp, unsigned nof_symbols) const override
  {
    symbol_grid_position position;
    if ((symbol_size == 0) || (nof_symbols == 0)) {
      return position;
    }
    const unsigned offset = static_cast<unsigned>(timestamp % symbol_size);
    if (offset != 0) {
      position.nof_samples_to_boundary = symbol_size - offset;
    }
    position.nof_samples = nof_symbols * symbol_size;
    position.nof_symbols = nof_symbols;
    return position;
  }

  const std::vector<entry_t>& get_entries() const { return entries; }

  void clear() { entries.clear(); }

private:
  std::vector<entry_t> entries;
  unsigned              symbol_size = 0;
};

class lower_phy_cfo_controller_spy : public lower_phy_cfo_controller
{
public:
  bool schedule_cfo_command(time_point time, float cfo_Hz, float cfo_drift_Hz_s) override { return false; }
};

class lower_phy_uplink_processor_spy : public lower_phy_uplink_processor, private lower_phy_center_freq_controller
{
public:
  lower_phy_uplink_processor_spy(const uplink_processor_configuration& config_) : config(config_) {}

  void connect(uplink_processor_notifier& notifier_,
               prach_processor_notifier&  prach_notifier_,
               puxch_processor_notifier&  puxch_notifier_) override
  {
    notifier       = &notifier_;
    prach_notifier = &prach_notifier_;
    puxch_notifier = &puxch_notifier_;
  }

  void stop() override {}

  lower_phy_cfo_controller& get_cfo_control() override { return cfo_processor_spy; }

  const uplink_processor_configuration& get_config() const { return config; }

  prach_processor_request_handler& get_prach_request_handler() override { return prach_req_handler_spy; }

  puxch_processor_request_handler& get_puxch_request_handler() override { return puxch_req_handler_spy; }

  uplink_processor_baseband& get_baseband() override { return uplink_proc_baseband_spy; }

  lower_phy_center_freq_controller& get_carrier_center_frequency_control() override { return *this; }

  uplink_processor_notifier* get_notifier() { return notifier; }

  prach_processor_notifier* get_prach_notifier() { return prach_notifier; }

  puxch_processor_notifier* get_puxch_notifier() { return puxch_notifier; }

  const uplink_processor_baseband_spy& get_uplink_proc_baseband_spy() const { return uplink_proc_baseband_spy; }

  /// Describes an OFDM symbol grid to the receive side (see uplink_processor_baseband_spy).
  void set_uplink_proc_baseband_symbol_size(unsigned size) { uplink_proc_baseband_spy.set_symbol_size(size); }

  const prach_processor_request_handler_spy& get_prach_req_handler_spy() const { return prach_req_handler_spy; }

  const puxch_processor_request_handler_spy& get_puxch_req_handler_spy() const { return puxch_req_handler_spy; }

  void clear()
  {
    notifier       = nullptr;
    prach_notifier = nullptr;
    puxch_notifier = nullptr;
    prach_req_handler_spy.clear();
    uplink_proc_baseband_spy.clear();
  }

private:
  // See interface for documentation.
  bool set_carrier_center_frequency(double carrier_center_frequency_Hz) override { return false; }

  uplink_processor_configuration      config;
  uplink_processor_notifier*          notifier       = nullptr;
  prach_processor_notifier*           prach_notifier = nullptr;
  puxch_processor_notifier*           puxch_notifier = nullptr;
  lower_phy_cfo_controller_spy        cfo_processor_spy;
  prach_processor_request_handler_spy prach_req_handler_spy;
  puxch_processor_request_handler_spy puxch_req_handler_spy;
  uplink_processor_baseband_spy       uplink_proc_baseband_spy;
};

class lower_phy_uplink_processor_factory_spy : public lower_phy_uplink_processor_factory
{
public:
  std::unique_ptr<lower_phy_uplink_processor> create(const uplink_processor_configuration& config) override
  {
    std::unique_ptr<lower_phy_uplink_processor_spy> proc = std::make_unique<lower_phy_uplink_processor_spy>(config);
    entries.emplace_back(proc.get());
    return proc;
  }

  std::vector<lower_phy_uplink_processor_spy*>& get_entries() { return entries; }

  void clear() { entries.clear(); }

private:
  std::vector<lower_phy_uplink_processor_spy*> entries;
};

} // namespace ocudu
