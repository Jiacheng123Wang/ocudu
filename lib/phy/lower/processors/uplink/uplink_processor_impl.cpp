// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "uplink_processor_impl.h"
#include "ocudu/gateways/baseband/buffer/baseband_gateway_buffer_reader_view.h"
#include "ocudu/ocuduvec/compare.h"
#include "ocudu/ocuduvec/conversion.h"
#include "ocudu/ocuduvec/copy.h"
#include "ocudu/ocuduvec/dot_prod.h"
#include "ocudu/phy/lower/lower_phy_baseband_metrics.h"
#include "ocudu/phy/lower/lower_phy_rx_symbol_context.h"
#include "ocudu/phy/lower/lower_phy_timing_context.h"
#include "ocudu/phy/lower/processors/uplink/prach/prach_processor_baseband.h"
#include "ocudu/phy/lower/processors/uplink/puxch/puxch_processor_baseband.h"
#include "ocudu/phy/lower/processors/uplink/uplink_processor_notifier.h"
#include "ocudu/support/math/stats.h"
#include <atomic>
#include <cstdio>

using namespace ocudu;

namespace {

/// \brief Host passes over the uplink samples that the CFO compensation still makes.
///
/// The compensation is the last step of the IQ -> LLR chain that converts every sample on the host
/// (int16 -> float -> int16 around the complex multiply). It only runs when an offset is in effect, so
/// a run whose round-trip count is zero is a run in which the samples went from the radio to the PRACH
/// and the PUxCH processors untouched - the contract the GPU pipeline mode has to satisfy (see
/// [ul_cfo]). A non-zero count with a non-zero offset is the NTN / `cfo` console case, where the
/// samples still have to be compensated for both consumers: that is the hard constraint the plan
/// names.
///
/// Compiled out - both the counters and the report - when the statistics probe is off, which is every
/// non-Apple-Silicon build.
class cfo_stats
{
#if defined(OCUDU_METAL_STATS)
  /// Symbols whose samples went through the round trip. Only symbols that were compensated reach it,
  /// so a zero count is the zero-glue state the GPU pipeline mode is supposed to be in.
  std::atomic<uint64_t> round_trips{0};
  /// Symbols processed, so a zero round-trip count can be told from "nothing ran".
  std::atomic<uint64_t> symbols{0};
#endif

public:
#if defined(OCUDU_METAL_STATS)
  void count_round_trip() { round_trips.fetch_add(1, std::memory_order_relaxed); }
  void count_symbol() { symbols.fetch_add(1, std::memory_order_relaxed); }

  /// Reports once at exit (the counters are a function-local static, see cfo_counters()). Silent when
  /// no symbol was processed, so the tools that merely link this library print nothing.
  ~cfo_stats()
  {
    uint64_t nof_symbols = symbols.load(std::memory_order_relaxed);
    if (nof_symbols == 0) {
      return;
    }
    std::fprintf(stderr,
                 "[ul_cfo] symbols=%llu round_trips=%llu\n",
                 static_cast<unsigned long long>(nof_symbols),
                 static_cast<unsigned long long>(round_trips.load(std::memory_order_relaxed)));
  }
#else
  void count_round_trip() {}
  void count_symbol() {}
#endif
};

/// Counters of this process, reported once at exit.
cfo_stats& cfo_counters()
{
  static cfo_stats s;
  return s;
}

} // namespace

lower_phy_uplink_processor_impl::lower_phy_uplink_processor_impl(std::unique_ptr<prach_processor> prach_proc_,
                                                                 std::unique_ptr<puxch_processor> puxch_proc_,
                                                                 const configuration&             config) :
  sector_id(config.sector_id),
  scs(config.scs),
  nof_rx_ports(config.nof_rx_ports),
  nof_slots_per_subframe(get_nof_slots_per_subframe(config.scs)),
  nof_symbols_per_slot(get_nsymb_per_slot(config.cp)),
  nof_samples_per_subframe(config.rate.to_kHz()),
  nof_symbols_per_subframe(nof_symbols_per_slot * get_nof_slots_per_subframe(config.scs)),
  symbol_buffer_write_index(0),
  current_symbol_index(0),
  prach_proc(std::move(prach_proc_)),
  puxch_proc(std::move(puxch_proc_)),
  cfo_processor(config.rate),
  temp_cf_buffer({2 * config.rate.get_dft_size(config.scs), config.nof_rx_ports})
{
  ocudu_assert(prach_proc, "Invalid PRACH processor.");
  ocudu_assert(puxch_proc, "Invalid PUxCH processor.");

  unsigned symbol_size_no_cp = config.rate.get_dft_size(config.scs);

  // Create the symbol buffers. The PUxCH processor tells how many symbols it can keep in flight, i.e.
  // how many buffers it needs, and every one of them is created with the maximum symbol size so that
  // resize() never has to grow (and hence never reallocate) the storage.
  unsigned nof_symbol_buffers = puxch_proc->get_baseband().get_nof_symbol_buffers();
  report_fatal_error_if_not(nof_symbol_buffers != 0, "The PUxCH processor requires no symbol buffer.");
  symbol_buffers.reserve(nof_symbol_buffers);
  for (unsigned i_buffer = 0; i_buffer != nof_symbol_buffers; ++i_buffer) {
    symbol_buffers.emplace_back(config.nof_rx_ports, 2 * symbol_size_no_cp);
  }

  // Setup symbol sizes.
  symbol_sizes.reserve(nof_symbols_per_subframe);
  unsigned sf_sample_count = 0;
  for (unsigned i_symbol = 0; i_symbol != nof_symbols_per_subframe; ++i_symbol) {
    unsigned cp_size     = config.cp.get_length(i_symbol, config.scs).to_samples(config.rate.to_Hz());
    unsigned symbol_size = cp_size + symbol_size_no_cp;
    symbol_sizes.emplace_back(symbol_size);
    sf_sample_count += symbol_size;
  }

  // Make sure the number of samples per subframe match the total number.
  report_fatal_error_if_not(sf_sample_count == nof_samples_per_subframe,
                            "The number of samples per subframe does not match the sampling rate.");
}

void lower_phy_uplink_processor_impl::connect(uplink_processor_notifier& notifier_,
                                              prach_processor_notifier&  prach_notifier,
                                              puxch_processor_notifier&  puxch_notifier)
{
  notifier = &notifier_;
  prach_proc->connect(prach_notifier);
  puxch_proc->connect(puxch_notifier);
}

prach_processor_request_handler& lower_phy_uplink_processor_impl::get_prach_request_handler()
{
  return prach_proc->get_request_handler();
}

puxch_processor_request_handler& lower_phy_uplink_processor_impl::get_puxch_request_handler()
{
  return puxch_proc->get_request_handler();
}

uplink_processor_baseband& lower_phy_uplink_processor_impl::get_baseband()
{
  return *this;
}

void lower_phy_uplink_processor_impl::process(const baseband_gateway_buffer_reader& samples,
                                              baseband_gateway_timestamp            timestamp)
{
  switch (state) {
    case fsm_states::alignment:
      process_alignment(samples, timestamp);
      break;
    case fsm_states::collecting:
      process_collecting(samples, timestamp);
      break;
  }
}

void lower_phy_uplink_processor_impl::process_alignment(const baseband_gateway_buffer_reader& samples,
                                                        baseband_gateway_timestamp            timestamp)
{
  // Calculate the sample index within a subframe.
  unsigned i_sample_sf = timestamp % nof_samples_per_subframe;
  unsigned nof_samples = samples.get_nof_samples();

  // Calculate the number of samples from the beginning of the buffer to the next subframe.
  unsigned nof_samples_next_sf = 0;
  if (i_sample_sf != 0) {
    nof_samples_next_sf = nof_samples_per_subframe - i_sample_sf;
  }

  // If the next subframe boundary is within the buffer, then process.
  if (nof_samples_next_sf < nof_samples) {
    baseband_gateway_buffer_reader_view samples2(samples, nof_samples_next_sf, nof_samples - nof_samples_next_sf);
    process_symbol_boundary(samples2, timestamp + nof_samples_next_sf);
    return;
  }

  // Otherwise, keep in state alignment.
  state = fsm_states::alignment;
}

void lower_phy_uplink_processor_impl::process_symbol_boundary(const baseband_gateway_buffer_reader& samples,
                                                              baseband_gateway_timestamp            timestamp)
{
  // Calculate the subframe index.
  unsigned i_sf = static_cast<uint64_t>((timestamp / nof_samples_per_subframe) % (NOF_SFNS * NOF_SUBFRAMES_PER_FRAME));

  // Calculate the sample index within the subframe.
  unsigned i_sample_sf = timestamp % nof_samples_per_subframe;

  // Calculate symbol index within the subframe and the sample index within the OFDM symbol.
  unsigned i_sample_symbol = i_sample_sf;
  unsigned i_symbol_sf     = 0;
  while (i_sample_symbol >= symbol_sizes[i_symbol_sf]) {
    i_sample_symbol -= symbol_sizes[i_symbol_sf];
    ++i_symbol_sf;
  }

  // If the sample is not aligned with the beginning of the OFDM symbol, align to next subframe.
  if (i_sample_symbol != 0) {
    process_alignment(samples, timestamp);
    return;
  }

  // Calculate system slot index and the symbol index within the slot.
  unsigned i_slot   = i_sf * nof_slots_per_subframe + i_symbol_sf / nof_symbols_per_slot;
  unsigned i_symbol = i_symbol_sf % nof_symbols_per_slot;

  // Create slot point.
  slot_point slot(to_numerology_value(scs), i_slot % (NOF_SFNS * NOF_SUBFRAMES_PER_FRAME * nof_slots_per_subframe));

  // Prepare current symbol context before collect samples.
  current_slot              = slot;
  current_symbol_index      = i_symbol;
  current_symbol_size       = symbol_sizes[i_symbol_sf];
  symbol_buffer_write_index = 0;
  current_symbol_timestamp  = timestamp;

  // Reserve the buffer this symbol is assembled in before writing into it. Acquiring it may finish
  // the oldest symbol still in flight, which is exactly what makes the buffer free: the samples of
  // the symbol being collected are therefore never written over samples a transform still reads.
  current_symbol_buffer = puxch_proc->get_baseband().acquire_symbol_buffer();
  symbol_buffers[current_symbol_buffer].resize(current_symbol_size);

  if (i_symbol == 0) {
    cfo_processor.next_cfo_command();
  }

  // Process baseband.
  process_collecting(samples, timestamp);
}

void lower_phy_uplink_processor_impl::process_collecting(const baseband_gateway_buffer_reader& samples,
                                                         baseband_gateway_timestamp            timestamp)
{
  ocudu_assert(notifier != nullptr, "Notifier has not been connected.");
  ocudu_assert(nof_rx_ports == samples.get_nof_channels(), "Invalid number of channels.");

  // Buffer the symbol being collected is assembled in.
  baseband_gateway_buffer_dynamic_aligned& symbol_buffer = symbol_buffers[current_symbol_buffer];

  // Check that the timestamp matches with the current sample timestamp.
  if ((current_symbol_timestamp + symbol_buffer_write_index) != timestamp) {
    // If the timestamp does not match, the alignment has been lost.
    process_alignment(samples, timestamp);
    return;
  }

  // Get the number of input samples.
  unsigned nof_input_samples = samples.get_nof_samples();

  // Select the minimum among the remainder of samples to process and the number of samples to complete the buffer.
  unsigned nof_samples = std::min(nof_input_samples, current_symbol_size - symbol_buffer_write_index);

  // For each port, concatenate samples.
  for (unsigned i_port = 0; i_port != nof_rx_ports; ++i_port) {
    // Select view of the symbol buffer.
    span<ci16_t> symbol_buffer_dst = symbol_buffer[i_port].subspan(symbol_buffer_write_index, nof_samples);

    // Select view of the input samples.
    span<const ci16_t> input_samples = samples.get_channel_buffer(i_port).first(nof_samples);

    // Append input samples into the symbol buffer.
    ocuduvec::copy(symbol_buffer_dst, input_samples);
  }

  // Increment the count of samples stored in the symbol buffer.
  symbol_buffer_write_index += nof_samples;

  // If the symbol buffer is not full, keep state in-sync and return.
  if (symbol_buffer_write_index < current_symbol_size) {
    state = fsm_states::collecting;
    return;
  }

  // Carrier frequency offset compensation.
  //
  // The processor only modifies the samples when it has an offset to apply. While it has none - its
  // state until something schedules a command (NTN Doppler compensation or the `cfo` console command,
  // see the RU controller) - the only thing left of this pass would be the int16 -> float -> int16
  // round trip, and that round trip is the exact identity (float(x) / 32767 * 32767 rounds back to x
  // for every int16, see baseband_cfo_processor_test). Skipping it hands the PRACH and the PUxCH
  // processors the samples exactly as the radio delivered them, byte for byte, and removes two host
  // passes over every sample of the uplink.
  if (cfo_processor.applies_compensation()) {
    cfo_counters().count_round_trip();
    // View over the temporary float-based complex samples for CFO processor.
    span<cf_t> view;
    for (unsigned i_channel = 0; i_channel != symbol_buffer.get_nof_channels(); ++i_channel) {
      // The CFO compensation is not currently supported for 16-bit complex integer samples. So, it must convert it to
      // single-precision complex floating-point samples.
      span<ci16_t> channel_buffer = symbol_buffer.get_writer().get_channel_buffer(i_channel);
      view                        = temp_cf_buffer.get_view({i_channel}).subspan(0, channel_buffer.size());
      ocuduvec::convert(view, channel_buffer, ocuduvec::scaling_factor_ci16_to_cf);
      cfo_processor.process(view);
      ocuduvec::convert(channel_buffer, view, ocuduvec::scaling_factor_cf_to_ci16);
    }
  }
  cfo_counters().count_symbol();

  // Advance CFO processor number of samples.
  cfo_processor.advance(symbol_buffer.get_nof_samples());

  // Process symbol by PRACH processor.
  prach_processor_baseband::symbol_context prach_context = {
      .slot = current_slot, .symbol = current_symbol_index, .sector = sector_id};
  prach_proc->get_baseband().process_symbol(symbol_buffer.get_reader(), prach_context);

  // Process symbol by PUxCH processor.
  lower_phy_rx_symbol_context puxch_context = {
      .slot = current_slot, .sector = sector_id, .nof_symbols = current_symbol_index};
  bool processed =
      puxch_proc->get_baseband().process_symbol(symbol_buffer.get_reader(), puxch_context, current_symbol_buffer);

  if (processed) {
    sample_statistics<float> avg_power;
    sample_statistics<float> peak_power;
    unsigned                 nof_channels = symbol_buffer.get_nof_channels();

    uint64_t total_processed_samples = 0;
    uint64_t nof_clipped_samples     = 0;

    // Process received signal before demodulation.
    for (unsigned i_channel = 0; i_channel != nof_channels; ++i_channel) {
      // Perform signal measurements on CI16 samples.
      span<const ci16_t> channel_buffer = symbol_buffer.get_reader().get_channel_buffer(i_channel);

      avg_power.update(ocuduvec::average_power(channel_buffer, ocuduvec::scaling_factor_ci16_to_cf));
      peak_power.update(ocuduvec::max_abs_element(channel_buffer, ocuduvec::scaling_factor_ci16_to_cf).second);
      nof_clipped_samples +=
          ocuduvec::count_if_part_abs_greater_than(channel_buffer, 0.95F, ocuduvec::scaling_factor_ci16_to_cf);
      total_processed_samples += channel_buffer.size();
    }

    lower_phy_baseband_metrics metrics = {.avg_power  = avg_power.get_mean(),
                                          .peak_power = peak_power.get_max(),
                                          .clipping =
                                              clipping_counters{.nof_clipped_samples   = nof_clipped_samples,
                                                                .nof_processed_samples = total_processed_samples}};
    notifier->on_new_metrics(metrics);
  }

  // Detect half-slot boundary.
  if (current_symbol_index == (nof_symbols_per_slot / 2) - 1) {
    // Notify half slot boundary.
    notifier->on_half_slot(lower_phy_timing_context{.slot = slot_point_extended(current_slot), .time_point = {}});
  }

  // Detect full slot boundary.
  if (current_symbol_index == nof_symbols_per_slot - 1) {
    // Notify full slot boundary.
    notifier->on_full_slot(lower_phy_timing_context{.slot = slot_point_extended(current_slot), .time_point = {}});
  }

  // Process next symbol with the remainder samples.
  baseband_gateway_buffer_reader_view samples2(samples, nof_samples, nof_input_samples - nof_samples);
  process_symbol_boundary(samples2, timestamp + nof_samples);
}

baseband_cfo_processor& lower_phy_uplink_processor_impl::get_cfo_control()
{
  return cfo_processor;
}

lower_phy_center_freq_controller& lower_phy_uplink_processor_impl::get_carrier_center_frequency_control()
{
  return puxch_proc->get_center_freq_control();
}
