// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "../../../../support/resource_grid_test_doubles.h"
#include "../../../modulation/ofdm_demodulator_test_doubles.h"
#include "puxch_processor_notifier_test_doubles.h"
#include "support/compare_sequences.h"
#include "ocudu/adt/format.h"
#include "ocudu/gateways/baseband/buffer/baseband_gateway_buffer_dynamic.h"
#include "ocudu/ocuduvec/conversion.h"
#include "ocudu/phy/lower/lower_phy_rx_symbol_context.h"
#include "ocudu/phy/lower/processors/uplink/puxch/puxch_processor_baseband.h"
#include "ocudu/phy/lower/processors/uplink/puxch/puxch_processor_request_handler.h"
#include "ocudu/phy/lower/processors/uplink/uplink_processor_baseband.h"
#include "ocudu/phy/lower/processors/uplink/uplink_processor_factories.h"
#include "fmt/ostream.h"
#include <gtest/gtest.h>
#include <random>

using namespace ocudu;

namespace ocudu {

std::ostream& operator<<(std::ostream& os, subcarrier_spacing scs)
{
  fmt::print(os, "{}", to_string(scs));
  return os;
}

std::ostream& operator<<(std::ostream& os, sampling_rate srate)
{
  fmt::print(os, "{} MHz", srate.to_MHz());
  return os;
}

std::ostream& operator<<(std::ostream& os, cyclic_prefix cp)
{
  fmt::print(os, "{}", cp.to_string());
  return os;
}

std::ostream& operator<<(std::ostream& os, span<const cf_t> data)
{
  fmt::print(os, "{}", data);
  return os;
}

std::ostream& operator<<(std::ostream& os, const lower_phy_rx_symbol_context& context)
{
  fmt::print(os, "{} {} {}", context.slot, context.nof_symbols, context.sector);
  return os;
}

std::ostream& operator<<(std::ostream& os, const ofdm_demodulator_configuration& config)
{
  fmt::print(os,
             "Numerology={} BW={} DftSize={} CP={} WindowOffset={} Scale={} CenterFreq={}Hz",
             config.numerology,
             config.bw_rb,
             config.dft_size,
             config.cp.to_string(),
             config.nof_samples_window_offset,
             config.scale,
             config.center_freq_Hz);
  return os;
}

bool operator==(const lower_phy_rx_symbol_context& left, const lower_phy_rx_symbol_context& right)
{
  return (left.slot == right.slot) && (left.nof_symbols == right.nof_symbols) && (left.sector == right.sector);
}

bool operator==(const ofdm_demodulator_configuration& left, const ofdm_demodulator_configuration& right)
{
  return (left.numerology == right.numerology) && (left.bw_rb == right.bw_rb) && (left.dft_size == right.dft_size) &&
         (left.cp == right.cp) && (left.nof_samples_window_offset == right.nof_samples_window_offset) &&
         (left.scale == right.scale) && (left.center_freq_Hz == right.center_freq_Hz);
}

bool operator==(const baseband_gateway_buffer_reader& left, const baseband_gateway_buffer_reader& right)
{
  if (left.get_nof_channels() != right.get_nof_channels()) {
    return false;
  }
  unsigned nof_channels = left.get_nof_channels();

  for (unsigned i_channel = 0; i_channel != nof_channels; ++i_channel) {
    span<const ci16_t> left_channel  = left.get_channel_buffer(i_channel);
    span<const ci16_t> right_channel = right.get_channel_buffer(i_channel);
    if (!std::equal(left_channel.begin(), left_channel.end(), right_channel.begin(), right_channel.end())) {
      return false;
    }
  }

  return true;
}

} // namespace ocudu

using LowerPhyUplinkProcessorParams = std::tuple<unsigned, sampling_rate, subcarrier_spacing, cyclic_prefix>;

namespace {

class LowerPhyUplinkProcessorFixture : public ::testing::TestWithParam<LowerPhyUplinkProcessorParams>
{
protected:
  static constexpr unsigned request_queue_size = 16;

  static void SetUpTestSuite()
  {
    if (puxch_proc_factory == nullptr) {
      ofdm_demod_factory_spy = std::make_shared<ofdm_demodulator_factory_spy>();
      ASSERT_NE(ofdm_demod_factory_spy, nullptr);

      puxch_proc_factory = create_puxch_processor_factory_sw(ofdm_demod_factory_spy);
      ASSERT_NE(puxch_proc_factory, nullptr);
    }
  }

  LowerPhyUplinkProcessorFixture() : rg_spy(rg_reader_spy, rg_writer_spy), shared_rg_spy(rg_spy) {}

  void SetUp() override
  {
    ASSERT_NE(puxch_proc_factory, nullptr);

    // Select parameters.
    unsigned           nof_rx_ports = std::get<0>(GetParam());
    sampling_rate      srate        = std::get<1>(GetParam());
    subcarrier_spacing scs          = std::get<2>(GetParam());
    cyclic_prefix      cp           = std::get<3>(GetParam());

    // Prepare configurations.
    config.cp                = cp;
    config.scs               = scs;
    config.srate             = srate;
    config.bandwidth_rb      = dist_bandwidth_prb(rgen);
    config.dft_window_offset = dist_dft_window_offset(rgen);
    config.center_freq_Hz    = dist_center_freq_Hz(rgen);
    config.nof_rx_ports      = nof_rx_ports;

    // Create processor.
    puxch_proc = puxch_proc_factory->create(config);
    ASSERT_NE(puxch_proc, nullptr);

    // Select OFDM demodulator processor spy.
    ofdm_demod_spy = ofdm_demod_factory_spy->get_demodulators().back();
  }

  static constexpr unsigned                            nof_frames_test = 3;
  static std::mt19937                                  rgen;
  static std::uniform_int_distribution<unsigned>       dist_sector_id;
  static std::uniform_int_distribution<unsigned>       dist_bandwidth_prb;
  static std::uniform_real_distribution<double>        dist_center_freq_Hz;
  static std::uniform_real_distribution<float>         dist_sample;
  static std::uniform_real_distribution<float>         dist_dft_window_offset;
  static std::shared_ptr<ofdm_demodulator_factory_spy> ofdm_demod_factory_spy;
  static std::shared_ptr<puxch_processor_factory>      puxch_proc_factory;

  puxch_processor_configuration    config;
  ofdm_symbol_demodulator_spy*     ofdm_demod_spy = nullptr;
  resource_grid_reader_spy         rg_reader_spy;
  resource_grid_writer_spy         rg_writer_spy;
  resource_grid_spy                rg_spy;
  shared_resource_grid_spy         shared_rg_spy;
  std::unique_ptr<puxch_processor> puxch_proc = nullptr;
};

std::mt19937                                  LowerPhyUplinkProcessorFixture::rgen(0);
std::uniform_int_distribution<unsigned>       LowerPhyUplinkProcessorFixture::dist_sector_id(0, 16);
std::uniform_int_distribution<unsigned>       LowerPhyUplinkProcessorFixture::dist_bandwidth_prb(1, MAX_NOF_PRBS);
std::uniform_real_distribution<double>        LowerPhyUplinkProcessorFixture::dist_center_freq_Hz(1e8, 6e9);
std::uniform_real_distribution<float>         LowerPhyUplinkProcessorFixture::dist_sample(-1, 1);
std::uniform_real_distribution<float>         LowerPhyUplinkProcessorFixture::dist_dft_window_offset(0, 0.9);
std::shared_ptr<ofdm_demodulator_factory_spy> LowerPhyUplinkProcessorFixture::ofdm_demod_factory_spy = nullptr;
std::shared_ptr<puxch_processor_factory>      LowerPhyUplinkProcessorFixture::puxch_proc_factory     = nullptr;

} // namespace

TEST_P(LowerPhyUplinkProcessorFixture, DemodulatorConfiguration)
{
  sampling_rate      srate = std::get<1>(GetParam());
  subcarrier_spacing scs   = std::get<2>(GetParam());
  cyclic_prefix      cp    = std::get<3>(GetParam());

  ofdm_demodulator_configuration expected_demod_config;
  expected_demod_config.numerology                = to_numerology_value(scs);
  expected_demod_config.bw_rb                     = config.bandwidth_rb;
  expected_demod_config.dft_size                  = srate.get_dft_size(scs);
  expected_demod_config.cp                        = cp;
  expected_demod_config.nof_samples_window_offset = static_cast<unsigned>(
      static_cast<float>(cp.get_length(1, scs).to_samples(srate.to_Hz())) * config.dft_window_offset);
  expected_demod_config.scale = 1.0F / static_cast<float>(std::sqrt(config.bandwidth_rb * NOF_SUBCARRIERS_PER_RB));
  expected_demod_config.center_freq_Hz = config.center_freq_Hz;

  ASSERT_EQ(ofdm_demod_spy->get_configuration(), expected_demod_config);
}

TEST_P(LowerPhyUplinkProcessorFixture, FlowNoRequest)
{
  unsigned           nof_rx_ports = std::get<0>(GetParam());
  sampling_rate      srate        = std::get<1>(GetParam());
  subcarrier_spacing scs          = std::get<2>(GetParam());
  cyclic_prefix      cp           = std::get<3>(GetParam());

  unsigned base_symbol_size = srate.get_dft_size(scs);

  baseband_gateway_buffer_dynamic buffer(nof_rx_ports, 2 * base_symbol_size);

  unsigned nof_symbols_per_slot   = get_nsymb_per_slot(cp);
  unsigned nof_slots_per_subframe = get_nof_slots_per_subframe(scs);

  // Create notifiers and connect.
  puxch_processor_notifier_spy puxch_proc_notifier_spy;
  puxch_proc->connect(puxch_proc_notifier_spy);

  slot_point slot(to_numerology_value(scs), 0);
  for (unsigned i_frame = 0; i_frame != nof_frames_test; ++i_frame) {
    for (unsigned i_subframe = 0; i_subframe != NOF_SUBFRAMES_PER_FRAME; ++i_subframe) {
      for (unsigned i_slot = 0, i_symbol_subframe = 0; i_slot != nof_slots_per_subframe; ++i_slot, ++slot) {
        for (unsigned i_symbol = 0; i_symbol != nof_symbols_per_slot; ++i_symbol, ++i_symbol_subframe) {
          unsigned cp_size = cp.get_length(i_symbol_subframe, scs).to_samples(srate.to_Hz());
          // Setup buffer.
          buffer.resize(cp_size + base_symbol_size);

          // Fill buffer.
          for (unsigned i_port = 0; i_port != nof_rx_ports; ++i_port) {
            span<ci16_t> port_buffer = buffer[i_port];
            std::generate(port_buffer.begin(), port_buffer.end(), []() {
              return to_ci16(cf_t(dist_sample(rgen) * INT16_MAX, dist_sample(rgen) * INT16_MAX));
            });
          }

          // Clear spies.
          puxch_proc_notifier_spy.clear_notifications();
          ofdm_demod_spy->clear_demodulate_entries();

          // Prepare expected PUxCH baseband entry context.
          lower_phy_rx_symbol_context puxch_context;
          puxch_context.slot        = slot;
          puxch_context.sector      = dist_sector_id(rgen);
          puxch_context.nof_symbols = i_symbol_subframe;

          // Process baseband.
          unsigned buffer_index = puxch_proc->get_baseband().acquire_symbol_buffer();
          puxch_proc->get_baseband().process_symbol(buffer.get_reader(), puxch_context, buffer_index, nullptr);

          // Assert OFDM demodulator call.
          auto& ofdm_demod_entries = ofdm_demod_spy->get_demodulate_entries();
          ASSERT_EQ(ofdm_demod_entries.size(), 0);

          // Assert notification.
          ASSERT_EQ(puxch_proc_notifier_spy.get_nof_notifications(), 0);
        }
      }
    }
  }
}

TEST_P(LowerPhyUplinkProcessorFixture, FlowFloodRequest)
{
  unsigned           nof_rx_ports = std::get<0>(GetParam());
  sampling_rate      srate        = std::get<1>(GetParam());
  subcarrier_spacing scs          = std::get<2>(GetParam());
  cyclic_prefix      cp           = std::get<3>(GetParam());

  unsigned base_symbol_size = srate.get_dft_size(scs);

  baseband_gateway_buffer_dynamic buffer(nof_rx_ports, 2 * base_symbol_size);

  unsigned nof_symbols_per_slot   = get_nsymb_per_slot(cp);
  unsigned nof_slots_per_subframe = get_nof_slots_per_subframe(scs);

  // Create notifiers and connect.
  puxch_processor_notifier_spy puxch_proc_notifier_spy;
  puxch_proc->connect(puxch_proc_notifier_spy);

  std::vector<ci16_t> ci16_buffer;

  slot_point slot(to_numerology_value(scs), 0);
  for (unsigned i_frame = 0; i_frame != nof_frames_test; ++i_frame) {
    for (unsigned i_subframe = 0; i_subframe != NOF_SUBFRAMES_PER_FRAME; ++i_subframe) {
      for (unsigned i_slot = 0, i_symbol_subframe = 0; i_slot != nof_slots_per_subframe; ++i_slot, ++slot) {
        resource_grid_context rg_context;
        rg_context.slot   = slot;
        rg_context.sector = dist_sector_id(rgen);

        // Request resource grid demodulation for the current slot.
        puxch_proc->get_request_handler().handle_request(shared_rg_spy.get_grid(), rg_context);

        for (unsigned i_symbol = 0; i_symbol != nof_symbols_per_slot; ++i_symbol, ++i_symbol_subframe) {
          unsigned cp_size = cp.get_length(i_symbol_subframe, scs).to_samples(srate.to_Hz());
          // Setup buffer.
          buffer.resize(cp_size + base_symbol_size);
          ci16_buffer.resize(cp_size + base_symbol_size);

          // Fill buffer.
          for (unsigned i_port = 0; i_port != nof_rx_ports; ++i_port) {
            span<ci16_t> port_buffer = buffer[i_port];
            std::generate(port_buffer.begin(), port_buffer.end(), []() {
              return to_ci16(cf_t(dist_sample(rgen) * INT16_MAX, dist_sample(rgen) * INT16_MAX));
            });
          }

          // Clear spies.
          puxch_proc_notifier_spy.clear_notifications();
          ofdm_demod_spy->clear_demodulate_entries();

          // Prepare expected PUxCH baseband entry context.
          lower_phy_rx_symbol_context puxch_context;
          puxch_context.slot        = rg_context.slot;
          puxch_context.sector      = rg_context.sector;
          puxch_context.nof_symbols = i_symbol;

          // Process baseband.
          unsigned buffer_index = puxch_proc->get_baseband().acquire_symbol_buffer();
          puxch_proc->get_baseband().process_symbol(buffer.get_reader(), puxch_context, buffer_index, nullptr);

          // Assert OFDM demodulator call.
          const auto& ofdm_demod_entries = ofdm_demod_spy->get_demodulate_entries();
          ASSERT_EQ(ofdm_demod_entries.size(), nof_rx_ports);
          for (unsigned i_port = 0; i_port != nof_rx_ports; ++i_port) {
            const auto& ofdm_demod_entry = ofdm_demod_entries[i_port];

            {
              error_type<std::string> compare_result =
                  compare_sequences(span<const ci16_t>(ofdm_demod_entry.input), span<const ci16_t>(buffer[i_port]));
              ASSERT_TRUE(compare_result.has_value()) << compare_result.error();
            }
            ASSERT_EQ(static_cast<const void*>(ofdm_demod_entry.grid), static_cast<const void*>(&rg_writer_spy));
            ASSERT_EQ(ofdm_demod_entry.port_index, i_port);
            ASSERT_EQ(ofdm_demod_entry.symbol_index, i_symbol_subframe);
          }

          // Assert notification.
          ASSERT_EQ(puxch_proc_notifier_spy.get_request_late().size(), 0);
          ASSERT_EQ(puxch_proc_notifier_spy.get_rx_symbol().size(), 1);
        }
      }
    }
  }
}

TEST_P(LowerPhyUplinkProcessorFixture, FlowPipelinedNotificationPerSymbol)
{
  const unsigned     nof_rx_ports = std::get<0>(GetParam());
  sampling_rate      srate        = std::get<1>(GetParam());
  subcarrier_spacing scs          = std::get<2>(GetParam());
  cyclic_prefix      cp           = std::get<3>(GetParam());

  const unsigned base_symbol_size = srate.get_dft_size(scs);
  const unsigned nof_symbols_per_slot   = get_nsymb_per_slot(cp);
  const unsigned nof_slots_per_subframe = get_nof_slots_per_subframe(scs);

  baseband_gateway_buffer_dynamic buffer(nof_rx_ports, 2 * base_symbol_size);

  // Run the demodulator in pipelined mode and connect the notifier. Every notification is
  // snapshotted together with the number of ports of that symbol that are already in the grid.
  ofdm_demod_spy->pipeline_depth = 2;

  struct notification_snapshot {
    unsigned symbol_index;
    unsigned finished_ports;
  };
  std::vector<notification_snapshot> notifications;

  puxch_processor_notifier_spy puxch_proc_notifier_spy;
  puxch_proc_notifier_spy.on_rx_symbol_hook = [&](const lower_phy_rx_symbol_context& context) {
    notifications.push_back({context.nof_symbols, ofdm_demod_spy->nof_finished_ports(context.nof_symbols)});
  };
  puxch_proc->connect(puxch_proc_notifier_spy);

  slot_point slot(to_numerology_value(scs), 0);
  for (unsigned i_frame = 0; i_frame != nof_frames_test; ++i_frame) {
    for (unsigned i_subframe = 0; i_subframe != NOF_SUBFRAMES_PER_FRAME; ++i_subframe) {
      for (unsigned i_slot = 0, i_symbol_subframe = 0; i_slot != nof_slots_per_subframe; ++i_slot, ++slot) {
        resource_grid_context rg_context;
        rg_context.slot   = slot;
        rg_context.sector = dist_sector_id(rgen);

        puxch_proc->get_request_handler().handle_request(shared_rg_spy.get_grid(), rg_context);
        ofdm_demod_spy->clear_pipeline();
        notifications.clear();

        for (unsigned i_symbol = 0; i_symbol != nof_symbols_per_slot; ++i_symbol, ++i_symbol_subframe) {
          const unsigned cp_size = cp.get_length(i_symbol_subframe, scs).to_samples(srate.to_Hz());
          buffer.resize(cp_size + base_symbol_size);
          for (unsigned i_port = 0; i_port != nof_rx_ports; ++i_port) {
            span<ci16_t> port_buffer = buffer[i_port];
            std::generate(port_buffer.begin(), port_buffer.end(), []() {
              return to_ci16(cf_t(dist_sample(rgen) * INT16_MAX, dist_sample(rgen) * INT16_MAX));
            });
          }

          lower_phy_rx_symbol_context puxch_context;
          puxch_context.slot        = rg_context.slot;
          puxch_context.sector      = rg_context.sector;
          puxch_context.nof_symbols = i_symbol;

          unsigned buffer_index = puxch_proc->get_baseband().acquire_symbol_buffer();
          puxch_proc->get_baseband().process_symbol(buffer.get_reader(), puxch_context, buffer_index, nullptr);
        }

        // Exactly one notification per OFDM symbol of the slot, in order (never one per port).
        ASSERT_EQ(notifications.size(), nof_symbols_per_slot);
        for (unsigned i_symbol = 0; i_symbol != nof_symbols_per_slot; ++i_symbol) {
          ASSERT_EQ(notifications[i_symbol].symbol_index, i_symbol)
              << "the symbols of a slot must be reported once each, in order";
          // ... and the symbol must be complete in the grid when it is reported.
          ASSERT_EQ(notifications[i_symbol].finished_ports, nof_rx_ports)
              << "symbol " << i_symbol << " was reported with a port still missing";
        }
        ASSERT_EQ(ofdm_demod_spy->get_demodulate_entries().size(), 0)
            << "the pipelined path must not demodulate synchronously";
      }
    }
  }
}

TEST_P(LowerPhyUplinkProcessorFixture, SymbolBuffersAreNotReusedWhileRead)
{
  const unsigned     nof_rx_ports = std::get<0>(GetParam());
  sampling_rate      srate        = std::get<1>(GetParam());
  subcarrier_spacing scs          = std::get<2>(GetParam());
  cyclic_prefix      cp           = std::get<3>(GetParam());

  const unsigned base_symbol_size     = srate.get_dft_size(scs);
  const unsigned nof_symbols_per_slot = get_nsymb_per_slot(cp);

  // The deepest pipeline there is: with a single receive port the buffers (one per symbol) and the
  // in-flight transforms are the same size, so every buffer gets wrapped around and the release
  // condition has to be exactly right. With more ports the pipeline holds fewer symbols per slot and
  // the same check is less tight, though still valid.
  ofdm_demod_spy->pipeline_depth = 8;

  baseband_gateway_buffer_dynamic buffer(nof_rx_ports, 2 * base_symbol_size);

  puxch_processor_notifier_spy puxch_proc_notifier_spy;
  puxch_proc->connect(puxch_proc_notifier_spy);

  const unsigned nof_symbol_buffers = puxch_proc->get_baseband().get_nof_symbol_buffers();

  // Symbol assembled in each buffer, or -1 when the buffer holds no symbol whose transform is in
  // flight. A symbol counts as in flight until every one of its ports has been finished - which is
  // what the demodulator spy counts, independently of when the processor releases the buffer.
  std::vector<int> symbol_of_buffer(nof_symbol_buffers, -1);

  // Drops the buffers of the symbols whose ports have all been finished. Called after every call that
  // may finish a transform (acquiring a buffer does, and so does processing a symbol).
  auto release_finished_buffers = [&]() {
    for (unsigned i_buffer = 0; i_buffer != nof_symbol_buffers; ++i_buffer) {
      if ((symbol_of_buffer[i_buffer] >= 0) &&
          (ofdm_demod_spy->nof_finished_ports(symbol_of_buffer[i_buffer]) == nof_rx_ports)) {
        symbol_of_buffer[i_buffer] = -1;
      }
    }
  };

  slot_point slot(to_numerology_value(scs), 0);
  for (unsigned i_slot = 0; i_slot != 2; ++i_slot, ++slot) {
    resource_grid_context rg_context;
    rg_context.slot   = slot;
    rg_context.sector = dist_sector_id(rgen);
    puxch_proc->get_request_handler().handle_request(shared_rg_spy.get_grid(), rg_context);
    ofdm_demod_spy->clear_pipeline();

    for (unsigned i_symbol = 0, i_symbol_subframe = 0; i_symbol != nof_symbols_per_slot;
         ++i_symbol, ++i_symbol_subframe) {
      const unsigned cp_size = cp.get_length(i_symbol_subframe, scs).to_samples(srate.to_Hz());
      buffer.resize(cp_size + base_symbol_size);
      for (unsigned i_port = 0; i_port != nof_rx_ports; ++i_port) {
        span<ci16_t> port_buffer = buffer[i_port];
        std::generate(port_buffer.begin(), port_buffer.end(), []() {
          return to_ci16(cf_t(dist_sample(rgen) * INT16_MAX, dist_sample(rgen) * INT16_MAX));
        });
      }

      lower_phy_rx_symbol_context puxch_context;
      puxch_context.slot        = rg_context.slot;
      puxch_context.sector      = rg_context.sector;
      puxch_context.nof_symbols = i_symbol;

      unsigned buffer_index = puxch_proc->get_baseband().acquire_symbol_buffer();

      // Acquiring may have finished the oldest transform - that is how a buffer becomes free - so the
      // mirror is brought up to date before the buffer is checked.
      release_finished_buffers();

      // The buffer must not hold a symbol that is still being transformed: reading the samples of
      // such a symbol is exactly what a transform does.
      ASSERT_EQ(symbol_of_buffer[buffer_index], -1)
          << "symbol buffer " << buffer_index << " was handed out for symbol " << i_symbol << " of slot "
          << slot.count() << " while the transform of symbol " << symbol_of_buffer[buffer_index]
          << " still reads it";
      symbol_of_buffer[buffer_index] = static_cast<int>(i_symbol);

      puxch_proc->get_baseband().process_symbol(buffer.get_reader(), puxch_context, buffer_index, nullptr);

      release_finished_buffers();
    }
  }
}

/// \brief The samples of a symbol stay alive until the transform reading them is finished.
///
/// The radio hands a receive buffer over and waits for it to come back (it keeps receiving into the
/// buffers it has, and a pool of four is what the SDR configuration gives it). The pipeline therefore
/// holds a reference per in-flight transform - the contract that lets the transform read the radio's
/// samples where they are instead of copying them per symbol (see rx_buffer_handle). The check is
/// two-sided on purpose: a pipeline that keeps NO reference would let the radio overwrite the samples
/// under a running transform, and one that keeps the reference after finishing would starve the pool.
TEST_P(LowerPhyUplinkProcessorFixture, SymbolSamplesAreKeptAliveUntilTheirTransformIsFinished)
{
  const unsigned     nof_rx_ports = std::get<0>(GetParam());
  sampling_rate      srate        = std::get<1>(GetParam());
  subcarrier_spacing scs          = std::get<2>(GetParam());
  cyclic_prefix      cp           = std::get<3>(GetParam());

  const unsigned base_symbol_size     = srate.get_dft_size(scs);
  const unsigned nof_symbols_per_slot = get_nsymb_per_slot(cp);

  // Depth two keeps a symbol in flight across the next submission, which is when a buffer released
  // too early would be reused; deeper pipelines behave the same way, only later.
  ofdm_demod_spy->pipeline_depth = 2;

  puxch_processor_notifier_spy puxch_proc_notifier_spy;
  puxch_proc->connect(puxch_proc_notifier_spy);

  resource_grid_context rg_context;
  rg_context.slot   = slot_point(to_numerology_value(scs), 0);
  rg_context.sector = dist_sector_id(rgen);
  puxch_proc->get_request_handler().handle_request(shared_rg_spy.get_grid(), rg_context);
  ofdm_demod_spy->clear_pipeline();

  std::vector<std::shared_ptr<baseband_gateway_buffer_dynamic_aligned>> owners;
  unsigned                                                              nof_kept_alive = 0;
  for (unsigned i_symbol = 0, i_symbol_subframe = 0; i_symbol != nof_symbols_per_slot;
       ++i_symbol, ++i_symbol_subframe) {
    const unsigned cp_size = cp.get_length(i_symbol_subframe, scs).to_samples(srate.to_Hz());

    // One receive buffer per symbol, as the radio delivers them.
    auto owner = std::make_shared<baseband_gateway_buffer_dynamic_aligned>(nof_rx_ports, 2 * base_symbol_size);
    owner->resize(cp_size + base_symbol_size);
    for (unsigned i_port = 0; i_port != nof_rx_ports; ++i_port) {
      span<ci16_t> port_buffer = (*owner)[i_port];
      std::generate(port_buffer.begin(), port_buffer.end(), []() {
        return to_ci16(cf_t(dist_sample(rgen) * INT16_MAX, dist_sample(rgen) * INT16_MAX));
      });
    }

    lower_phy_rx_symbol_context puxch_context;
    puxch_context.slot        = rg_context.slot;
    puxch_context.sector      = rg_context.sector;
    puxch_context.nof_symbols = i_symbol;

    unsigned buffer_index = puxch_proc->get_baseband().acquire_symbol_buffer();
    puxch_proc->get_baseband().process_symbol(owner->get_reader(), puxch_context, buffer_index, owner);

    if (owner.use_count() > 1) {
      ++nof_kept_alive;
    }
    owners.push_back(std::move(owner));
  }

  // The pipeline held the samples of every symbol it had not finished yet...
  ASSERT_GT(nof_kept_alive, 0) << "the pipeline kept no reference at all: nothing was protected";
  // ... and dropped all of them when the slot's last symbol drained it: the radio owns the buffers
  // again, which is what keeps its pool from starving.
  for (unsigned i_symbol = 0; i_symbol != owners.size(); ++i_symbol) {
    ASSERT_EQ(owners[i_symbol].use_count(), 1)
        << "the samples of finished symbol " << i_symbol << " are still held by the pipeline";
  }
}

/// \brief The same contract when the backend runs its transforms LATER than finish_symbol() (D1, 5.9.7).
///
/// A backend that defers - the fused lane's single submission does - cannot rely on the pipeline's own
/// references: those are dropped when the slot drains, and the transforms have not run yet. The samples then
/// have to be held by the BACKEND and given back when its block completes. This is the guard the air leg
/// lacked: without it the transforms read samples the radio had already overwritten, measured as
/// `crc=KO 942 / OK 46` at `sinr=37.6 dB` (design document 5.9.7).
///
/// Two-sided on purpose, like the test above: samples held for too short corrupt the transform, and samples
/// held for too long starve the radio's pool of four.
TEST_P(LowerPhyUplinkProcessorFixture, DeferredTransformsKeepTheirSamplesUntilTheirBlockCompletes)
{
  const unsigned     nof_rx_ports = std::get<0>(GetParam());
  sampling_rate      srate        = std::get<1>(GetParam());
  subcarrier_spacing scs          = std::get<2>(GetParam());
  cyclic_prefix      cp           = std::get<3>(GetParam());

  const unsigned base_symbol_size     = srate.get_dft_size(scs);
  const unsigned nof_symbols_per_slot = get_nsymb_per_slot(cp);

  ofdm_demod_spy->pipeline_depth   = 2;
  ofdm_demod_spy->defers_execution = true;

  puxch_processor_notifier_spy puxch_proc_notifier_spy;
  puxch_proc->connect(puxch_proc_notifier_spy);

  resource_grid_context rg_context;
  rg_context.slot   = slot_point(to_numerology_value(scs), 0);
  rg_context.sector = dist_sector_id(rgen);
  puxch_proc->get_request_handler().handle_request(shared_rg_spy.get_grid(), rg_context);
  ofdm_demod_spy->clear_pipeline();

  std::vector<std::shared_ptr<baseband_gateway_buffer_dynamic_aligned>> owners;
  for (unsigned i_symbol = 0, i_symbol_subframe = 0; i_symbol != nof_symbols_per_slot;
       ++i_symbol, ++i_symbol_subframe) {
    const unsigned cp_size = cp.get_length(i_symbol_subframe, scs).to_samples(srate.to_Hz());

    auto owner = std::make_shared<baseband_gateway_buffer_dynamic_aligned>(nof_rx_ports, 2 * base_symbol_size);
    owner->resize(cp_size + base_symbol_size);
    for (unsigned i_port = 0; i_port != nof_rx_ports; ++i_port) {
      span<ci16_t> port_buffer = (*owner)[i_port];
      std::generate(port_buffer.begin(), port_buffer.end(), []() {
        return to_ci16(cf_t(dist_sample(rgen) * INT16_MAX, dist_sample(rgen) * INT16_MAX));
      });
    }

    lower_phy_rx_symbol_context puxch_context;
    puxch_context.slot        = rg_context.slot;
    puxch_context.sector      = rg_context.sector;
    puxch_context.nof_symbols = i_symbol;

    unsigned buffer_index = puxch_proc->get_baseband().acquire_symbol_buffer();
    puxch_proc->get_baseband().process_symbol(owner->get_reader(), puxch_context, buffer_index, owner);

    owners.push_back(std::move(owner));
  }

  // The backend was handed one token per transform, and it is holding every symbol's samples: the slot has
  // drained, so nothing else does.
  ASSERT_EQ(ofdm_demod_spy->nof_retained(), nof_symbols_per_slot * nof_rx_ports)
      << "the backend that defers was not handed a token per transform";
  for (unsigned i_symbol = 0; i_symbol != owners.size(); ++i_symbol) {
    ASSERT_GT(owners[i_symbol].use_count(), 1)
        << "the samples of symbol " << i_symbol << " were let go at the slot's end, while the deferred "
           "transforms that read them had not run yet";
  }

  // The block completes: the samples go back to the radio, all of them.
  ofdm_demod_spy->complete_deferred_block();
  EXPECT_FALSE(ofdm_demod_spy->has_retained()) << "the backend still holds a token after its block completed";
  for (unsigned i_symbol = 0; i_symbol != owners.size(); ++i_symbol) {
    ASSERT_EQ(owners[i_symbol].use_count(), 1)
        << "the samples of symbol " << i_symbol << " are still held after the deferred block completed";
  }
}

TEST_P(LowerPhyUplinkProcessorFixture, LateRequest)
{
  unsigned           sector_id    = dist_sector_id(rgen);
  unsigned           nof_rx_ports = std::get<0>(GetParam());
  sampling_rate      srate        = std::get<1>(GetParam());
  subcarrier_spacing scs          = std::get<2>(GetParam());
  cyclic_prefix      cp           = std::get<3>(GetParam());

  unsigned base_symbol_size = srate.get_dft_size(scs);

  baseband_gateway_buffer_dynamic buffer(nof_rx_ports, 2 * base_symbol_size);
  std::vector<ci16_t>             ci16_buffer;

  unsigned nof_symbols_per_slot   = get_nsymb_per_slot(cp);
  unsigned nof_slots_per_subframe = get_nof_slots_per_subframe(scs);

  // Create notifiers and connect.
  puxch_processor_notifier_spy puxch_proc_notifier_spy;
  puxch_proc->connect(puxch_proc_notifier_spy);

  unsigned initial_slot = 3;
  unsigned late_slot    = 2;
  unsigned next_slot    = 4;

  shared_resource_grid shared_rg = shared_rg_spy.get_grid();

  // Initial request.
  resource_grid_context initial_rg_context;
  initial_rg_context.slot   = slot_point(to_numerology_value(scs), initial_slot);
  initial_rg_context.sector = sector_id;
  puxch_proc->get_request_handler().handle_request(shared_rg.copy(), initial_rg_context);

  // Late request.
  resource_grid_context late_rg_context;
  late_rg_context.slot   = slot_point(to_numerology_value(scs), late_slot);
  late_rg_context.sector = sector_id;
  puxch_proc->get_request_handler().handle_request(shared_rg.copy(), late_rg_context);

  // Next request.
  resource_grid_context next_rg_context;
  next_rg_context.slot   = slot_point(to_numerology_value(scs), next_slot);
  next_rg_context.sector = sector_id;
  puxch_proc->get_request_handler().handle_request(shared_rg.copy(), next_rg_context);

  for (unsigned i_subframe = 0; i_subframe != NOF_SUBFRAMES_PER_FRAME; ++i_subframe) {
    for (unsigned i_slot = 0, i_symbol_subframe = 0; i_slot != nof_slots_per_subframe; ++i_slot) {
      // Process one frame.
      for (unsigned i_symbol = 0; i_symbol != nof_symbols_per_slot; ++i_symbol, ++i_symbol_subframe) {
        unsigned cp_size = cp.get_length(i_symbol_subframe, scs).to_samples(srate.to_Hz());
        // Setup buffer.
        buffer.resize(cp_size + base_symbol_size);
        ci16_buffer.resize(cp_size + base_symbol_size);

        // Fill buffer.
        for (unsigned i_port = 0; i_port != nof_rx_ports; ++i_port) {
          span<ci16_t> port_buffer = buffer[i_port];
          std::generate(port_buffer.begin(), port_buffer.end(), []() {
            return to_ci16(cf_t(dist_sample(rgen) * INT16_MAX, dist_sample(rgen) * INT16_MAX));
          });
        }

        // Clear spies.
        puxch_proc_notifier_spy.clear_notifications();
        ofdm_demod_spy->clear_demodulate_entries();

        // Prepare expected PUxCH baseband entry context.
        lower_phy_rx_symbol_context puxch_context;
        puxch_context.slot        = slot_point(to_numerology_value(scs), i_slot);
        puxch_context.sector      = sector_id;
        puxch_context.nof_symbols = i_symbol;

        // Process baseband.
        unsigned buffer_index = puxch_proc->get_baseband().acquire_symbol_buffer();
        puxch_proc->get_baseband().process_symbol(buffer.get_reader(), puxch_context, buffer_index, nullptr);

        // Assert OFDM demodulator call only for initial and next slot.
        const auto& ofdm_demod_entries = ofdm_demod_spy->get_demodulate_entries();
        if ((i_slot == initial_slot) || (i_slot == next_slot)) {
          resource_grid_spy* rg_spy_ptr = &rg_spy;
          ASSERT_EQ(ofdm_demod_entries.size(), nof_rx_ports);
          for (unsigned i_port = 0; i_port != nof_rx_ports; ++i_port) {
            const auto& ofdm_demod_entry = ofdm_demod_entries[i_port];

            {
              error_type<std::string> compare_result =
                  compare_sequences(span<const ci16_t>(ofdm_demod_entry.input), span<const ci16_t>(buffer[i_port]));
              ASSERT_TRUE(compare_result.has_value()) << compare_result.error();
            }
            ASSERT_EQ(static_cast<const void*>(ofdm_demod_entry.grid), static_cast<const void*>(rg_spy_ptr));
            ASSERT_EQ(ofdm_demod_entry.port_index, i_port);
            ASSERT_EQ(ofdm_demod_entry.symbol_index, i_symbol_subframe);
          }
        } else {
          ASSERT_EQ(ofdm_demod_entries.size(), 0);
        }

        // Assert notifications.
        if (i_slot == next_slot) {
          const auto& lates = puxch_proc_notifier_spy.get_request_late();
          ASSERT_EQ(lates.size(), 1);
          ASSERT_EQ(lates.front().slot, late_rg_context.slot);
          ASSERT_EQ(lates.front().sector, late_rg_context.sector);
        } else {
          ASSERT_EQ(puxch_proc_notifier_spy.get_request_late().size(), 0);
        }
        if ((i_slot == initial_slot) || (i_slot == next_slot)) {
          ASSERT_EQ(puxch_proc_notifier_spy.get_rx_symbol().size(), 1);
        }
      }
    }
  }
}

TEST_P(LowerPhyUplinkProcessorFixture, OverflowRequest)
{
  static constexpr unsigned nof_overflow_entries = 3;
  unsigned                  sector_id            = dist_sector_id(rgen);
  unsigned                  nof_rx_ports         = std::get<0>(GetParam());
  sampling_rate             srate                = std::get<1>(GetParam());
  subcarrier_spacing        scs                  = std::get<2>(GetParam());

  unsigned base_symbol_size = srate.get_dft_size(scs);

  baseband_gateway_buffer_dynamic buffer(nof_rx_ports, 2 * base_symbol_size);

  // Create notifiers and connect.
  puxch_processor_notifier_spy puxch_proc_notifier_spy;
  puxch_proc->connect(puxch_proc_notifier_spy);

  shared_resource_grid shared_rg = shared_rg_spy.get_grid();

  // Generate requests.
  slot_point slot(to_numerology_value(scs), 0);
  for (unsigned i_request = 0; i_request != request_queue_size + nof_overflow_entries; ++i_request) {
    resource_grid_context rg_context;
    rg_context.slot   = slot + i_request;
    rg_context.sector = sector_id;
    puxch_proc->get_request_handler().handle_request(shared_rg.copy(), rg_context);

    unsigned nof_expected_late = (i_request >= request_queue_size) ? (i_request - request_queue_size + 1) : 0;
    ASSERT_EQ(puxch_proc_notifier_spy.get_rx_symbol().size(), 0);
    ASSERT_EQ(puxch_proc_notifier_spy.get_request_late().size(), nof_expected_late);
  }
}

// Creates test suite that combines all possible parameters.
INSTANTIATE_TEST_SUITE_P(LowerPhyUplinkProcessor,
                         LowerPhyUplinkProcessorFixture,
                         ::testing::Combine(::testing::Values(1, 2, 4),
                                            ::testing::Values(sampling_rate::from_MHz(3.84),
                                                              sampling_rate::from_MHz(7.68)),
                                            ::testing::Values(subcarrier_spacing::kHz15, subcarrier_spacing::kHz30),
                                            ::testing::Values(cyclic_prefix::NORMAL, cyclic_prefix::EXTENDED)));
