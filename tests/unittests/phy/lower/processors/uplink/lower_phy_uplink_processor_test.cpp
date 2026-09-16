// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "prach/prach_processor_notifier_test_doubles.h"
#include "prach/prach_processor_test_doubles.h"
#include "puxch/puxch_processor_notifier_test_doubles.h"
#include "puxch/puxch_processor_test_doubles.h"
#include "support/compare_sequences.h"
#include "uplink_processor_notifier_test_doubles.h"
#include "ocudu/adt/format.h"
#include "ocudu/gateways/baseband/buffer/baseband_gateway_buffer_dynamic.h"
#include "ocudu/gateways/baseband/buffer/baseband_gateway_buffer_reader_view.h"
#include "ocudu/phy/lower/processors/uplink/uplink_processor_baseband.h"
#include "ocudu/phy/lower/processors/uplink/uplink_processor_factories.h"
#include "ocudu/ran/resource_block.h"
#include "fmt/ostream.h"
#include <gtest/gtest.h>
#include <memory>
#include <random>

using namespace ocudu;

namespace ocudu {

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

std::ostream& operator<<(std::ostream& os, const sampling_rate& srate)
{
  fmt::print(os, "{}", srate);
  return os;
}

std::ostream& operator<<(std::ostream& os, const subcarrier_spacing& scs)
{
  fmt::print(os, "{}", to_string(scs));
  return os;
}

std::ostream& operator<<(std::ostream& os, cyclic_prefix cp)
{
  fmt::print(os, "{}", cp.to_string());
  return os;
}

std::ostream& operator<<(std::ostream& os, const prach_processor_baseband::symbol_context& context)
{
  fmt::print(os, "{} {} {}", context.slot, context.symbol, context.sector);
  return os;
}

std::ostream& operator<<(std::ostream& os, const puxch_processor_configuration& config)
{
  fmt::print(os,
             "CP={} SCS={} SRate={} BW={} DftWindowOffset={} CenterFreq={}Hz NofRxPorts={}",
             config.cp,
             to_string(config.scs),
             config.srate,
             config.bandwidth_rb,
             config.dft_window_offset,
             config.center_freq_Hz,
             config.nof_rx_ports);
  return os;
}

bool operator==(const prach_processor_baseband::symbol_context left,
                const prach_processor_baseband::symbol_context right)
{
  return (left.slot == right.slot) && (left.symbol == right.symbol) && (left.sector == right.sector);
}

bool operator==(const lower_phy_rx_symbol_context left, const lower_phy_rx_symbol_context right)
{
  return (left.slot == right.slot) && (left.nof_symbols == right.nof_symbols) && (left.sector == right.sector);
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

bool operator==(const puxch_processor_configuration& left, const puxch_processor_configuration& right)
{
  return (left.cp == right.cp) && (left.scs == right.scs) && (left.srate == right.srate) &&
         (left.bandwidth_rb == right.bandwidth_rb) && (left.dft_window_offset == right.dft_window_offset) &&
         (left.center_freq_Hz == right.center_freq_Hz) && (left.nof_rx_ports == right.nof_rx_ports);
}

} // namespace ocudu

using LowerPhyUplinkProcessorParams = std::tuple<unsigned, sampling_rate, subcarrier_spacing, cyclic_prefix>;

namespace {

class LowerPhyUplinkProcessorFixture : public ::testing::TestWithParam<LowerPhyUplinkProcessorParams>
{
protected:
  static void SetUpTestSuite()
  {
    if (ul_proc_factory == nullptr) {
      prach_proc_factory = std::make_shared<prach_processor_factory_spy>();
      ASSERT_NE(prach_proc_factory, nullptr);

      puxch_proc_factory = std::make_shared<puxch_processor_factory_spy>();
      ASSERT_NE(puxch_proc_factory, nullptr);

      ul_proc_factory = create_uplink_processor_factory_sw(prach_proc_factory, puxch_proc_factory);
      ASSERT_NE(ul_proc_factory, nullptr);
    }
  }

  void SetUp() override
  {
    ASSERT_NE(ul_proc_factory, nullptr);

    // Select parameters.
    unsigned           nof_rx_ports = std::get<0>(GetParam());
    sampling_rate      srate        = std::get<1>(GetParam());
    subcarrier_spacing scs          = std::get<2>(GetParam());
    cyclic_prefix      cp           = std::get<3>(GetParam());

    // Prepare configurations.
    config.sector_id           = 0;
    config.scs                 = scs;
    config.cp                  = cp;
    config.rate                = srate;
    config.bandwidth_prb       = dist_bandwidth_prb(rgen);
    config.center_frequency_Hz = dist_center_freq_Hz(rgen);
    config.nof_rx_ports        = nof_rx_ports;

    // Create processor.
    ul_processor = ul_proc_factory->create(config);
    ASSERT_NE(ul_processor, nullptr);

    // Select PRACH processor spy.
    prach_proc_spy = &prach_proc_factory->get_spy();

    // Select PUxCH processor spy.
    puxch_proc_spy = &puxch_proc_factory->get_spy();
  }

  static constexpr unsigned                                  nof_frames_test = 10;
  static std::mt19937                                        rgen;
  static std::uniform_int_distribution<unsigned>             dist_bandwidth_prb;
  static std::uniform_real_distribution<double>              dist_center_freq_Hz;
  static std::uniform_real_distribution<float>               dist_sample;
  static std::shared_ptr<prach_processor_factory_spy>        prach_proc_factory;
  static std::shared_ptr<puxch_processor_factory_spy>        puxch_proc_factory;
  static std::shared_ptr<lower_phy_uplink_processor_factory> ul_proc_factory;

  uplink_processor_configuration              config;
  std::unique_ptr<lower_phy_uplink_processor> ul_processor   = nullptr;
  prach_processor_spy*                        prach_proc_spy = nullptr;
  puxch_processor_spy*                        puxch_proc_spy = nullptr;
};

std::mt19937                                        LowerPhyUplinkProcessorFixture::rgen(0);
std::uniform_int_distribution<unsigned>             LowerPhyUplinkProcessorFixture::dist_bandwidth_prb(1, MAX_NOF_PRBS);
std::uniform_real_distribution<double>              LowerPhyUplinkProcessorFixture::dist_center_freq_Hz(1e8, 6e9);
std::uniform_real_distribution<float>               LowerPhyUplinkProcessorFixture::dist_sample(-1, 1);
std::shared_ptr<prach_processor_factory_spy>        LowerPhyUplinkProcessorFixture::prach_proc_factory = nullptr;
std::shared_ptr<puxch_processor_factory_spy>        LowerPhyUplinkProcessorFixture::puxch_proc_factory = nullptr;
std::shared_ptr<lower_phy_uplink_processor_factory> LowerPhyUplinkProcessorFixture::ul_proc_factory    = nullptr;

} // namespace

TEST_P(LowerPhyUplinkProcessorFixture, PuxchConfiguration)
{
  puxch_processor_configuration expected_puxch_config;
  expected_puxch_config.cp                = config.cp;
  expected_puxch_config.scs               = config.scs;
  expected_puxch_config.srate             = config.rate;
  expected_puxch_config.bandwidth_rb      = config.bandwidth_prb;
  expected_puxch_config.dft_window_offset = 0.5;
  expected_puxch_config.center_freq_Hz    = config.center_frequency_Hz;
  expected_puxch_config.nof_rx_ports      = config.nof_rx_ports;
  ASSERT_EQ(expected_puxch_config, puxch_proc_spy->get_configuration());
}

TEST_P(LowerPhyUplinkProcessorFixture, Flow)
{
  unsigned           nof_rx_ports = std::get<0>(GetParam());
  sampling_rate      srate        = std::get<1>(GetParam());
  subcarrier_spacing scs          = std::get<2>(GetParam());
  cyclic_prefix      cp           = std::get<3>(GetParam());

  unsigned base_symbol_size = srate.get_dft_size(scs);

  // Every call hands over the samples of exactly one OFDM symbol, so the processor reads them where
  // the radio put them: no symbol buffer is acquired for the whole run (a symbol buffer is only needed
  // by a symbol that straddles two blocks, see FlowStraddlingBlocks).
  baseband_gateway_buffer_dynamic buffer(nof_rx_ports, 2 * base_symbol_size);

  unsigned nof_symbols_per_slot   = get_nsymb_per_slot(cp);
  unsigned nof_slots_per_subframe = get_nof_slots_per_subframe(scs);

  // Create notifiers and connect.
  uplink_processor_notifier_spy uplink_proc_notifier_spy;
  prach_processor_notifier_spy  prach_proc_notifier_spy;
  puxch_processor_notifier_spy  puxch_proc_notifier_spy;
  ul_processor->connect(uplink_proc_notifier_spy, prach_proc_notifier_spy, puxch_proc_notifier_spy);

  uplink_processor_baseband& ul_proc_baseband = ul_processor->get_baseband();

  // The buffer the radio would hand over: one per call, kept alive by the processor until the
  // transforms reading it are finished (see uplink_processor_baseband::rx_buffer_handle).
  auto rx_owner = std::make_shared<baseband_gateway_buffer_dynamic_aligned>(nof_rx_ports, 2 * base_symbol_size);

  baseband_gateway_timestamp timestamp = 0;
  for (unsigned i_frame = 0, i_slot_frame = 0; i_frame != nof_frames_test; ++i_frame) {
    for (unsigned i_subframe = 0; i_subframe != NOF_SUBFRAMES_PER_FRAME; ++i_subframe) {
      for (unsigned i_slot = 0, i_symbol_subframe = 0; i_slot != nof_slots_per_subframe; ++i_slot, ++i_slot_frame) {
        for (unsigned i_symbol = 0; i_symbol != nof_symbols_per_slot; ++i_symbol, ++i_symbol_subframe) {
          // Calculate cyclic prefix size in samples.
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
          uplink_proc_notifier_spy.clear_notifications();
          prach_proc_notifier_spy.clear_notifications();
          puxch_proc_notifier_spy.clear_notifications();
          prach_proc_spy->clear();
          puxch_proc_spy->clear();

          // Process baseband.
          ul_proc_baseband.process(buffer.get_reader(), timestamp, rx_owner);

          // Prepare expected PRACH baseband entry context.
          prach_processor_baseband::symbol_context prach_context;
          prach_context.slot   = slot_point(to_numerology_value(scs), i_slot_frame);
          prach_context.symbol = i_symbol;
          prach_context.sector = config.sector_id;

          // Prepare expected PUxCH baseband entry context.
          lower_phy_rx_symbol_context puxch_context;
          puxch_context.slot        = slot_point(to_numerology_value(scs), i_slot_frame);
          puxch_context.sector      = config.sector_id;
          puxch_context.nof_symbols = i_symbol;

          // Assert PRACH processor call.
          auto& prach_proc_entries = prach_proc_spy->get_baseband_entries();
          ASSERT_EQ(prach_proc_entries.size(), 1);
          auto& prach_proc_entry = prach_proc_entries.back();
          ASSERT_EQ(prach_proc_entry.context, prach_context);
          {
            baseband_gateway_buffer_read_only expected(buffer.get_reader());
            for (unsigned channel = 0; channel != prach_proc_entry.samples.get_nof_channels(); ++channel) {
              error_type<std::string> compare_result = compare_sequences(
                  prach_proc_entry.samples.get_channel_buffer(channel), expected.get_channel_buffer(channel));
              ASSERT_TRUE(compare_result.has_value()) << compare_result.error();
            }
          }

          // Assert PUxCH processor call.
          auto& puxch_proc_entries = puxch_proc_spy->get_baseband_entries();
          ASSERT_EQ(puxch_proc_entries.size(), 1);
          auto& puxch_proc_entry = puxch_proc_entries.back();
          ASSERT_EQ(puxch_proc_entry.context, puxch_context);
          // The samples of the symbol lie entirely in the block just handed over, so they must reach
          // the PUxCH where the radio put them: no symbol buffer, no copy.
          ASSERT_FALSE(puxch_proc_entry.buffer_index.has_value())
              << "symbol " << i_symbol << " of slot " << i_slot_frame
              << " was assembled instead of read where the radio put it";
          // The samples the transform will read are the samples of the block, byte for byte: the slice
          // is not a re-interpretation of them.
          {
            baseband_gateway_buffer_read_only expected(buffer.get_reader());
            for (unsigned channel = 0; channel != puxch_proc_entry.samples.get_nof_channels(); ++channel) {
              error_type<std::string> compare_result = compare_sequences(
                  puxch_proc_entry.samples.get_channel_buffer(channel), expected.get_channel_buffer(channel));
              ASSERT_TRUE(compare_result.has_value()) << compare_result.error();
            }
          }
          // The handle the caller passed must reach the pipeline (it is what keeps the samples alive
          // there): it is the ONLY lifetime of an in-place symbol, so dropping it is invisible until
          // the radio reuses a buffer under a running transform.
          ASSERT_EQ(puxch_proc_entry.owner, rx_owner) << "the receive buffer handle did not reach the PUxCH";

          // No PRACH or PUxCH notifications.
          ASSERT_EQ(prach_proc_notifier_spy.get_nof_notifications(), 0);
          ASSERT_EQ(puxch_proc_notifier_spy.get_nof_notifications(), 0);

          const auto& half_slot_entries = uplink_proc_notifier_spy.get_half_slots();
          if (i_symbol == nof_symbols_per_slot / 2 - 1) {
            ASSERT_EQ(half_slot_entries.size(), 1);
          } else {
            ASSERT_EQ(half_slot_entries.size(), 0);
          }

          const auto& full_slot_entries = uplink_proc_notifier_spy.get_full_slots();
          if (i_symbol == nof_symbols_per_slot - 1) {
            ASSERT_EQ(full_slot_entries.size(), 1);
          } else {
            ASSERT_EQ(full_slot_entries.size(), 0);
          }

          // Increment timestamp.
          timestamp += cp_size + base_symbol_size;
        }
      }
    }
  }

  // Not a single symbol buffer was acquired in the whole run: every symbol of it was read in place.
  ASSERT_EQ(puxch_proc_spy->get_nof_acquired_buffers(), 0);
}

/// A symbol whose samples straddle two radio blocks cannot be read where they lie (they are not
/// contiguous in memory): the processor assembles it in the symbol buffer acquire_symbol_buffer()
/// hands out, and the transforms read that buffer. This is the path the single-packet and half-slot
/// receive policies still use (see lower_phy_configuration), so it must keep working - and it is
/// checked here for the samples it hands over, not only for the buffer pairing.
TEST_P(LowerPhyUplinkProcessorFixture, FlowStraddlingBlocks)
{
  const unsigned     nof_rx_ports      = std::get<0>(GetParam());
  const sampling_rate srate            = std::get<1>(GetParam());
  const subcarrier_spacing scs         = std::get<2>(GetParam());
  const cyclic_prefix cp               = std::get<3>(GetParam());
  const unsigned     base_symbol_size  = srate.get_dft_size(scs);
  const unsigned     nof_symbols_slot  = get_nsymb_per_slot(cp);

  uplink_processor_notifier_spy uplink_proc_notifier_spy;
  prach_processor_notifier_spy  prach_proc_notifier_spy;
  puxch_processor_notifier_spy  puxch_proc_notifier_spy;
  ul_processor->connect(uplink_proc_notifier_spy, prach_proc_notifier_spy, puxch_proc_notifier_spy);
  uplink_processor_baseband& ul_proc_baseband = ul_processor->get_baseband();

  const unsigned nof_symbol_buffers = puxch_proc_spy->get_baseband().get_nof_symbol_buffers();
  auto           rx_owner = std::make_shared<baseband_gateway_buffer_dynamic_aligned>(nof_rx_ports, 2 * base_symbol_size);
  baseband_gateway_buffer_dynamic buffer(nof_rx_ports, 2 * base_symbol_size);

  puxch_proc_spy->clear();
  baseband_gateway_timestamp timestamp = 0;
  unsigned                   expected_buffer = 0;
  for (unsigned i_symbol = 0, i_symbol_subframe = 0; i_symbol != nof_symbols_slot; ++i_symbol, ++i_symbol_subframe) {
    const unsigned symbol_size = cp.get_length(i_symbol_subframe, scs).to_samples(srate.to_Hz()) + base_symbol_size;
    buffer.resize(symbol_size);
    for (unsigned i_port = 0; i_port != nof_rx_ports; ++i_port) {
      span<ci16_t> port_buffer = buffer[i_port];
      std::generate(port_buffer.begin(), port_buffer.end(), []() {
        return to_ci16(cf_t(dist_sample(rgen) * INT16_MAX, dist_sample(rgen) * INT16_MAX));
      });
    }

    // The radio block ends in the middle of the symbol: the processor can neither process it in place
    // nor leave it for the next call, so it starts assembling it.
    const unsigned first_block = symbol_size / 2;
    ul_proc_baseband.process(baseband_gateway_buffer_reader_view(buffer.get_reader(), 0, first_block),
                             timestamp,
                             rx_owner);
    ASSERT_EQ(puxch_proc_spy->get_baseband_entries().size(), i_symbol)
        << "an incomplete symbol reached the PUxCH";
    ASSERT_EQ(puxch_proc_spy->get_nof_acquired_buffers(), i_symbol + 1);

    // The next block completes it.
    ul_proc_baseband.process(
        baseband_gateway_buffer_reader_view(buffer.get_reader(), first_block, symbol_size - first_block),
        timestamp + first_block,
        rx_owner);

    auto& puxch_entries = puxch_proc_spy->get_baseband_entries();
    ASSERT_EQ(puxch_entries.size(), i_symbol + 1);
    auto& entry = puxch_entries.back();
    ASSERT_TRUE(entry.buffer_index.has_value()) << "a straddling symbol must be assembled in a symbol buffer";
    ASSERT_EQ(*entry.buffer_index, expected_buffer)
        << "symbol " << i_symbol << " was not assembled in the symbol buffer that was acquired";
    expected_buffer = (expected_buffer + 1) % nof_symbol_buffers;
    ASSERT_EQ(entry.owner, rx_owner);

    // The assembled samples are the samples of the two blocks, in order.
    baseband_gateway_buffer_read_only expected(buffer.get_reader());
    for (unsigned channel = 0; channel != entry.samples.get_nof_channels(); ++channel) {
      error_type<std::string> compare_result =
          compare_sequences(entry.samples.get_channel_buffer(channel), expected.get_channel_buffer(channel));
      ASSERT_TRUE(compare_result.has_value()) << compare_result.error();
    }

    timestamp += symbol_size;
  }
}

/// The in-place path is not a different processing of the samples, it is the same processing without
/// the copy: the samples handed to the PUxCH must be identical whether they were read where the radio
/// put them or assembled first. This is the property the whole leg rests on - one slot through both
/// paths, symbol by symbol, compared byte for byte.
TEST_P(LowerPhyUplinkProcessorFixture, BothSamplePathsHandOverTheSameSamples)
{
  const unsigned     nof_rx_ports     = std::get<0>(GetParam());
  const sampling_rate srate           = std::get<1>(GetParam());
  const subcarrier_spacing scs        = std::get<2>(GetParam());
  const cyclic_prefix cp              = std::get<3>(GetParam());
  const unsigned     base_symbol_size = srate.get_dft_size(scs);
  const unsigned     nof_symbols_slot = get_nsymb_per_slot(cp);

  uplink_processor_notifier_spy uplink_proc_notifier_spy;
  prach_processor_notifier_spy  prach_proc_notifier_spy;
  puxch_processor_notifier_spy  puxch_proc_notifier_spy;
  ul_processor->connect(uplink_proc_notifier_spy, prach_proc_notifier_spy, puxch_proc_notifier_spy);
  uplink_processor_baseband& ul_proc_baseband = ul_processor->get_baseband();

  auto rx_owner = std::make_shared<baseband_gateway_buffer_dynamic_aligned>(nof_rx_ports, 2 * base_symbol_size);

  // One slot of samples, generated once: both runs must process exactly these.
  std::vector<unsigned>                          symbol_sizes(nof_symbols_slot);
  std::vector<std::vector<std::vector<ci16_t>>>  samples(nof_symbols_slot);
  for (unsigned i_symbol = 0, i_symbol_subframe = 0; i_symbol != nof_symbols_slot; ++i_symbol, ++i_symbol_subframe) {
    symbol_sizes[i_symbol] = cp.get_length(i_symbol_subframe, scs).to_samples(srate.to_Hz()) + base_symbol_size;
    samples[i_symbol].resize(nof_rx_ports);
    for (unsigned i_port = 0; i_port != nof_rx_ports; ++i_port) {
      samples[i_symbol][i_port].resize(symbol_sizes[i_symbol]);
      std::generate(samples[i_symbol][i_port].begin(), samples[i_symbol][i_port].end(), []() {
        return to_ci16(cf_t(dist_sample(rgen) * INT16_MAX, dist_sample(rgen) * INT16_MAX));
      });
    }
  }

  // Runs one slot through the processor and returns the samples the PUxCH was handed, per symbol.
  auto run_slot = [&](bool straddling) {
    std::vector<std::vector<std::vector<ci16_t>>> handed_over(nof_symbols_slot);
    baseband_gateway_buffer_dynamic               buffer(nof_rx_ports, 2 * base_symbol_size);
    baseband_gateway_timestamp                    timestamp = 0;
    puxch_proc_spy->clear();

    for (unsigned i_symbol = 0; i_symbol != nof_symbols_slot; ++i_symbol) {
      buffer.resize(symbol_sizes[i_symbol]);
      for (unsigned i_port = 0; i_port != nof_rx_ports; ++i_port) {
        ocuduvec::copy(buffer[i_port], span<const ci16_t>(samples[i_symbol][i_port]));
      }

      if (straddling) {
        const unsigned first_block = symbol_sizes[i_symbol] / 2;
        ul_proc_baseband.process(baseband_gateway_buffer_reader_view(buffer.get_reader(), 0, first_block),
                                 timestamp,
                                 rx_owner);
        ul_proc_baseband.process(baseband_gateway_buffer_reader_view(
                                     buffer.get_reader(), first_block, symbol_sizes[i_symbol] - first_block),
                                 timestamp + first_block,
                                 rx_owner);
      } else {
        ul_proc_baseband.process(buffer.get_reader(), timestamp, rx_owner);
      }
      timestamp += symbol_sizes[i_symbol];

      auto& entries = puxch_proc_spy->get_baseband_entries();
      EXPECT_EQ(entries.size(), i_symbol + 1);
      auto& entry = entries.back();
      EXPECT_EQ(entry.buffer_index.has_value(), straddling);
      handed_over[i_symbol].resize(nof_rx_ports);
      for (unsigned i_port = 0; i_port != nof_rx_ports; ++i_port) {
        span<const ci16_t> channel = entry.samples.get_channel_buffer(i_port);
        handed_over[i_symbol][i_port].assign(channel.begin(), channel.end());
      }
    }
    return handed_over;
  };

  std::vector<std::vector<std::vector<ci16_t>>> in_place  = run_slot(false);
  std::vector<std::vector<std::vector<ci16_t>>> assembled = run_slot(true);

  for (unsigned i_symbol = 0; i_symbol != nof_symbols_slot; ++i_symbol) {
    for (unsigned i_port = 0; i_port != nof_rx_ports; ++i_port) {
      ASSERT_EQ(in_place[i_symbol][i_port], samples[i_symbol][i_port])
          << "the in-place path did not hand over the samples of symbol " << i_symbol;
      ASSERT_EQ(assembled[i_symbol][i_port], samples[i_symbol][i_port])
          << "the assembly path did not hand over the samples of symbol " << i_symbol;
      ASSERT_EQ(in_place[i_symbol][i_port], assembled[i_symbol][i_port])
          << "the two paths handed different samples over for symbol " << i_symbol;
    }
  }
}

TEST_P(LowerPhyUplinkProcessorFixture, MetricsAreMeasuredOnlyForAConsumer)
{
  const unsigned     nof_rx_ports = std::get<0>(GetParam());
  sampling_rate      srate        = std::get<1>(GetParam());
  subcarrier_spacing scs          = std::get<2>(GetParam());
  cyclic_prefix      cp           = std::get<3>(GetParam());

  const unsigned base_symbol_size     = srate.get_dft_size(scs);
  const unsigned nof_symbols_per_slot = get_nsymb_per_slot(cp);

  // Two processors that differ only in whether the baseband metrics are consumed: the measurement is
  // three passes over every sample of every symbol (average power, peak power, clipping) and nothing
  // reads its result unless the application exposes an RU metrics collector for the sector.
  uplink_processor_configuration consuming_config = config;
  consuming_config.metrics_enabled                = true;
  uplink_processor_configuration ignored_config   = config;
  ignored_config.metrics_enabled                  = false;

  std::unique_ptr<lower_phy_uplink_processor> consuming_processor = ul_proc_factory->create(consuming_config);
  std::unique_ptr<lower_phy_uplink_processor> ignoring_processor  = ul_proc_factory->create(ignored_config);
  ASSERT_NE(consuming_processor, nullptr);
  ASSERT_NE(ignoring_processor, nullptr);

  // One slot of samples through a processor, symbol by symbol, exactly like the radio delivers them.
  auto run_one_slot = [&](lower_phy_uplink_processor& processor, uplink_processor_notifier_spy& notifier) {
    prach_processor_notifier_spy prach_notifier;
    puxch_processor_notifier_spy puxch_notifier;
    processor.connect(notifier, prach_notifier, puxch_notifier);

    baseband_gateway_buffer_dynamic buffer(nof_rx_ports, 2 * base_symbol_size);
    baseband_gateway_timestamp      timestamp = 0;
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
      processor.get_baseband().process(buffer.get_reader(), timestamp, nullptr);
      timestamp += cp_size + base_symbol_size;
    }
  };

  uplink_processor_notifier_spy consuming_notifier;
  uplink_processor_notifier_spy ignoring_notifier;
  run_one_slot(*consuming_processor, consuming_notifier);
  run_one_slot(*ignoring_processor, ignoring_notifier);

  // The consuming processor measures one set per processed symbol...
  ASSERT_EQ(consuming_notifier.get_metrics().size(), nof_symbols_per_slot);
  // ... and the other one measures nothing at all, while having processed the same slot (the full
  // slot notification is what proves the samples went through: a zero count on its own could just
  // mean the run never happened).
  ASSERT_EQ(ignoring_notifier.get_metrics().size(), 0);
  ASSERT_EQ(ignoring_notifier.get_full_slots().size(), 1);
  ASSERT_EQ(consuming_notifier.get_full_slots().size(), 1);
}

// Creates test suite that combines all possible parameters.
INSTANTIATE_TEST_SUITE_P(LowerPhyUplinkProcessor,
                         LowerPhyUplinkProcessorFixture,
                         ::testing::Combine(::testing::Values(1, 2),
                                            ::testing::Values(sampling_rate::from_MHz(7.68)),
                                            ::testing::Values(subcarrier_spacing::kHz15,
                                                              subcarrier_spacing::kHz30,
                                                              subcarrier_spacing::kHz60),
                                            ::testing::Values(cyclic_prefix::NORMAL, cyclic_prefix::EXTENDED)));
