// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/ocuduvec/copy.h"
#include "ocudu/phy/lower/modulation/modulation_factories.h"
#include "ocudu/support/ocudu_test.h"
#include <random>

namespace ocudu {

class ofdm_symbol_demodulator_spy : public ofdm_symbol_demodulator
{
public:
  struct demodulate_entry {
    std::vector<ci16_t>         input;
    const resource_grid_writer* grid;
    unsigned                    port_index;
    unsigned                    symbol_index;
  };

  ofdm_symbol_demodulator_spy(const ofdm_demodulator_configuration& config) :
    rgen(0), dist(-1, +1), configuration(config)
  {
    // Do nothing.
  }

  void set_center_frequency(double center_frequency_Hz) override {}

  unsigned get_symbol_size(unsigned symbol_index) const override
  {
    unsigned sampling_rate_Hz = (configuration.dft_size * 15000) << configuration.numerology;
    return configuration.cp.get_length(symbol_index, to_subcarrier_spacing(configuration.numerology))
               .to_samples(sampling_rate_Hz) +
           configuration.dft_size;
  }

  void
  demodulate(resource_grid_writer& grid, span<const ci16_t> input, unsigned port_index, unsigned symbol_index) override
  {
    TESTASSERT_EQ(input.size(), get_symbol_size(symbol_index));

    demodulate_entries.emplace_back();
    demodulate_entry& entry = demodulate_entries.back();
    entry.grid              = &grid;
    entry.port_index        = port_index;
    entry.symbol_index      = symbol_index;

    entry.input.resize(input.size());
    ocuduvec::copy(entry.input, input);
  }

  void clear_demodulate_entries() { demodulate_entries.clear(); }

  // ---- Optional transform pipeline: with pipeline_depth > 1 the caller uses submit_symbol() and
  // finish_symbol() instead of demodulate(). The spy records which (port, symbol) each slot holds
  // and which ones have already been finished, so a test can check the order in which the upper PHY
  // is notified.
  unsigned get_pipeline_depth() const override { return pipeline_depth; }

  void submit_symbol(resource_grid_writer& /*grid*/,
                     span<const ci16_t> /*input*/,
                     unsigned port_index,
                     unsigned symbol_index,
                     unsigned slot) override
  {
    if (pipeline_slots.size() <= slot) {
      pipeline_slots.resize(slot + 1, {});
    }
    // Store the index within the slot: the notifier reports the slot-relative symbol index.
    pipeline_slots[slot] = {port_index, symbol_index % get_nsymb_per_slot(configuration.cp)};
    ++nof_submitted;
  }

  void finish_symbol(resource_grid_writer& /*grid*/, unsigned slot) override
  {
    if (slot >= pipeline_slots.size()) {
      return;
    }
    finished_slots.push_back(pipeline_slots[slot]);
  }

  // ---- A backend that runs its transforms LATER than finish_symbol() (D1, design document 5.9.7) -----
  //
  // The fused lane's single submission does exactly this: the slot's transforms are encoded into the hop's
  // command buffer, which the LANE commits - so the samples they read have to outlive finish_symbol(), and
  // that call is what tells the radio they may be recycled. The spy records the tokens the puxch hands over
  // and can run them ("the block completed"), so a test judges the lifetime instead of the mechanism.
  bool defers_transform_execution() const override { return defers_execution; }

  bool retain_input(void (*release)(void* context), void* context) override
  {
    if (!defers_execution) {
      return false;
    }
    retained.push_back({release, context});
    return true;
  }

  /// Number of tokens handed over so far.
  unsigned nof_retained() const { return static_cast<unsigned>(retained.size()); }

  /// Whether any token is still held (i.e. a receive buffer the radio cannot use again).
  bool has_retained() const { return !retained.empty(); }

  /// Runs every token's release exactly once, as the deferred block's completion would.
  void complete_deferred_block()
  {
    std::vector<token_entry> tokens;
    tokens.swap(retained);
    for (const token_entry& token : tokens) {
      token.release(token.context);
    }
  }

  /// Makes this backend defer its transforms (see the interface documentation).
  bool defers_execution = false;

  /// Number of notifications reported to the upper PHY through finished symbols (cumulative, so a
  /// test can compare it across calls even when the notifier spy is cleared).
  unsigned get_nof_notifications() const { return nof_notifications; }

  /// Accounts one reported symbol (called by the notifier spy of the test through the hook below).
  void count_notification() { ++nof_notifications; }

  /// Number of finished (port, symbol) pairs of \c symbol_index.
  unsigned nof_finished_ports(unsigned symbol_index) const
  {
    unsigned count = 0;
    for (const pipeline_entry& entry : finished_slots) {
      count += (entry.symbol_index == symbol_index) ? 1 : 0;
    }
    return count;
  }

  void clear_pipeline()
  {
    pipeline_slots.clear();
    finished_slots.clear();
    nof_submitted = 0;
  }

  unsigned pipeline_depth   = 1;
  unsigned nof_submitted    = 0;
  unsigned nof_notifications = 0;

  const ofdm_demodulator_configuration& get_configuration() const { return configuration; }

  const std::vector<demodulate_entry>& get_demodulate_entries() const { return demodulate_entries; }

private:
  /// (port, symbol) held by one transform slot of the pipeline.
  struct pipeline_entry {
    unsigned port_index   = 0;
    unsigned symbol_index = 0;
  };

  /// One piece of input the backend holds until its deferred block completes (see retain_input()).
  struct token_entry {
    void (*release)(void* context);
    void* context;
  };

  std::vector<token_entry> retained;

  std::mt19937                          rgen;
  std::uniform_real_distribution<float> dist;
  ofdm_demodulator_configuration        configuration;
  std::vector<demodulate_entry>         demodulate_entries;
  std::vector<pipeline_entry>           pipeline_slots;
  std::vector<pipeline_entry>           finished_slots;
};

class ofdm_demodulator_factory_spy : public ofdm_demodulator_factory
{
public:
  std::unique_ptr<ofdm_symbol_demodulator>
  create_ofdm_symbol_demodulator(const ofdm_demodulator_configuration& config) override
  {
    std::unique_ptr<ofdm_symbol_demodulator_spy> ptr = std::make_unique<ofdm_symbol_demodulator_spy>(config);
    demodulators.push_back(ptr.get());
    return ptr;
  }
  std::unique_ptr<ofdm_slot_demodulator>
  create_ofdm_slot_demodulator(const ofdm_demodulator_configuration& config) override
  {
    return nullptr;
  }

  std::vector<ofdm_symbol_demodulator_spy*>& get_demodulators() { return demodulators; }

private:
  std::vector<ofdm_symbol_demodulator_spy*> demodulators;
};

} // namespace ocudu
