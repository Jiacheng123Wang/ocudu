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
#include "ocudu/phy/phy_pipeline_contract.h"
#include "ocudu/support/math/stats.h"
#include <atomic>
#include <cstdio>

using namespace ocudu;

namespace {

/// \brief Host passes over the uplink samples that this processor still makes, per symbol.
///
/// The samples of a symbol are copied into the symbol buffer once (the assembly, which the radio's
/// block-based delivery and the PRACH's need for a contiguous host buffer require), and - while they
/// are there - two optional passes may still run over them on the host:
///
///   * the CFO compensation, which converts them to complex float and back. It is skipped unless an
///     offset is in effect, so a run whose round-trip count is zero sent the samples from the radio to
///     the PRACH and the PUxCH processors untouched. A non-zero count with a non-zero offset is the
///     NTN / `cfo` console case, where both consumers need the compensated samples: the hard
///     constraint the plan names.
///   * the baseband metrics (average power, peak power, clipping), three passes per sample that only
///     an application collecting RU metrics reads.
///
/// The report is what makes the pipeline mode's contract checkable on air: with the GPU pipeline and
/// no frequency offset to apply, both counts are zero while \c symbols is not - which tells a real
/// zero from a run that never happened. \c cfo_commands says whether anything ever asked for a
/// compensation at all.
///
/// Compiled out - both the counters and the report - when the statistics probe is off, which is every
/// non-Apple-Silicon build.
class ul_host_stats
{
#if defined(OCUDU_METAL_STATS)
  /// Symbols whose samples went through the round trip. Only symbols that were compensated reach it,
  /// so a zero count is the zero-glue state the GPU pipeline mode is supposed to be in.
  std::atomic<uint64_t> round_trips{0};
  /// Symbols processed, so a zero round-trip count can be told from "nothing ran".
  std::atomic<uint64_t> symbols{0};
  /// Commands accepted by the compensation, i.e. how many times anything asked for an offset. Zero
  /// with a zero offset means "no controller ever asked" (a terrestrial cell); a non-zero count with
  /// a zero offset means "a controller asked for exactly 0 Hz" (an NTN cell with no Doppler).
  std::atomic<uint64_t> commands{0};
  /// Symbols whose samples were measured for the baseband metrics.
  std::atomic<uint64_t> metrics{0};
  /// Offset in effect, in hertz, as last observed.
  std::atomic<float> cfo_hz{0.0F};
  /// Symbols whose assembly started in a symbol buffer (one per symbol; counted at the symbol
  /// boundary, so a symbol whose samples arrive in two radio blocks counts once). It can exceed
  /// \c symbols by the number of symbols that were started and never completed. This is the host pass
  /// over the IQ samples that the GPU pipeline mode is meant to remove (see the design document).
  std::atomic<uint64_t> assembled{0};
  /// Whether the baseband metrics are consumed in this run (see the probe's own contract check).
  std::atomic<bool> metrics_consumed{false};
#endif

public:
#if defined(OCUDU_METAL_STATS)
  void count_round_trip() { round_trips.fetch_add(1, std::memory_order_relaxed); }
  void count_symbol() { symbols.fetch_add(1, std::memory_order_relaxed); }
  void count_metrics() { metrics.fetch_add(1, std::memory_order_relaxed); }
  void count_assembled() { assembled.fetch_add(1, std::memory_order_relaxed); }
  /// Sticky: several processors may share the process (a gNB has one per sector, tests build more),
  /// and the check asks whether ANY of them measured for a consumer.
  void set_metrics_consumed(bool consumed)
  {
    if (consumed) {
      metrics_consumed.store(true, std::memory_order_relaxed);
    }
  }

  uint64_t get_round_trips() const { return round_trips.load(std::memory_order_relaxed); }
  uint64_t get_commands() const { return commands.load(std::memory_order_relaxed); }
  uint64_t get_symbols() const { return symbols.load(std::memory_order_relaxed); }
  uint64_t get_metrics() const { return metrics.load(std::memory_order_relaxed); }
  uint64_t get_assembled() const { return assembled.load(std::memory_order_relaxed); }
  float    get_cfo_hz() const { return cfo_hz.load(std::memory_order_relaxed); }
  bool     get_metrics_consumed() const { return metrics_consumed.load(std::memory_order_relaxed); }

  /// Samples the state of the compensation (called once per processed symbol).
  void observe(float cfo_Hz, uint64_t nof_commands)
  {
    uint64_t seen = commands.load(std::memory_order_relaxed);
    while ((nof_commands > seen) &&
           !commands.compare_exchange_weak(seen, nof_commands, std::memory_order_relaxed)) {
    }
    cfo_hz.store(cfo_Hz, std::memory_order_relaxed);
  }

  /// Reports once at exit (the counters are a function-local static, see ul_host_counters()). Silent
  /// when no symbol was processed, so the tools that merely link this library print nothing.
  ~ul_host_stats()
  {
    uint64_t nof_symbols = symbols.load(std::memory_order_relaxed);
    if (nof_symbols == 0) {
      return;
    }
    std::fprintf(stderr,
                 "[ul_host] symbols=%llu cfo_round_trips=%llu cfo_commands=%llu cfo_hz=%.3f metrics=%llu "
                 "assembled=%llu\n",
                 static_cast<unsigned long long>(nof_symbols),
                 static_cast<unsigned long long>(round_trips.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(commands.load(std::memory_order_relaxed)),
                 static_cast<double>(cfo_hz.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(metrics.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(assembled.load(std::memory_order_relaxed)));
  }
#else
  void count_round_trip() {}
  void count_symbol() {}
  void count_metrics() {}
  void count_assembled() {}
  void set_metrics_consumed(bool /*consumed*/) {}
  void observe(float /*cfo_Hz*/, uint64_t /*nof_commands*/) {}
#endif
};

/// Counters of this process, reported once at exit.
ul_host_stats& ul_host_counters()
{
  static ul_host_stats s;
  return s;
}

#if defined(OCUDU_METAL_STATS)
/// \brief Registers this processor's requirements with the pipeline contract (see
/// phy_pipeline_contract.h): the host touches the samples only for the reasons a mode allows.
///
/// Compiled with the counters it reads: without the statistics probe there is nothing to evaluate -
/// and a build configuration that failed to compile because a probe was off is exactly the defect
/// S-7g-5 fixed (a macOS build without ENABLE_METAL_STATS is the default one).
void register_ul_host_contract_checks()
{
  // Nothing may convert the samples unless an offset is in effect: the round trip exists to apply the
  // CFO, and a run whose offset is zero must not pay for it (S-7g-1).
  register_phy_pipeline_check(
      {"cfo compensation", []() -> std::optional<bool> {
         const ul_host_stats& c           = ul_host_counters();
         uint64_t             round_trips = c.get_round_trips();
         uint64_t             commands    = c.get_commands();
         float                cfo_hz      = c.get_cfo_hz();
         std::fprintf(stderr,
                      "%llu round trips over %llu symbols, %llu commands, offset %.3f Hz",
                      static_cast<unsigned long long>(round_trips),
                      static_cast<unsigned long long>(c.get_symbols()),
                      static_cast<unsigned long long>(commands),
                      static_cast<double>(cfo_hz));
         // Round trips without an offset in effect would be pure waste; with an offset they are the
         // compensation both the PRACH and the PUxCH need on the host (the hard constraint the plan
         // names), so they are legitimate.
         return (round_trips == 0) || (cfo_hz != 0.0F);
       }});

  // The baseband metrics are measured only for an application that reads them (S-7g-2).
  register_phy_pipeline_check(
      {"baseband metrics", []() -> std::optional<bool> {
         const ul_host_stats& c        = ul_host_counters();
         uint64_t             measured = c.get_metrics();
         bool                 consumed = c.get_metrics_consumed();
         std::fprintf(stderr,
                      "%llu symbols measured for %llu processed (metrics %s)",
                      static_cast<unsigned long long>(measured),
                      static_cast<unsigned long long>(c.get_symbols()),
                      consumed ? "enabled" : "disabled");
         return (measured == 0) || consumed;
       }});

  // The last host pass over the samples. Reported in every mode (the number is on the [ul_host] line
  // too), but only the fused GPU mode claims to have removed it: with the CPU pipeline or the
  // module-level offload the samples legitimately travel through the host.
  register_phy_pipeline_check(
      {"host sample assembly", []() -> std::optional<bool> {
         const ul_host_stats& c         = ul_host_counters();
         uint64_t              assembled = c.get_assembled();
         phy_pipeline_mode     mode      = phy_pipeline_mode_registry::get();
         std::fprintf(stderr,
                      "%llu symbols copied into a symbol buffer (mode=%s)",
                      static_cast<unsigned long long>(assembled),
                      to_string(mode));
         if (mode != phy_pipeline_mode::gpu) {
           return std::nullopt;
         }
         return assembled == 0;
       }});
}
#endif // OCUDU_METAL_STATS

} // namespace

lower_phy_uplink_processor_impl::lower_phy_uplink_processor_impl(std::unique_ptr<prach_processor> prach_proc_,
                                                                 std::unique_ptr<puxch_processor> puxch_proc_,
                                                                 const configuration&             config) :
  sector_id(config.sector_id),
  scs(config.scs),
  nof_rx_ports(config.nof_rx_ports),
  metrics_enabled(config.metrics_enabled),
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

#if defined(OCUDU_METAL_STATS)
  // Publish this processor's requirements to the pipeline contract (once per process).
  static const bool checks_registered = []() {
    register_ul_host_contract_checks();
    return true;
  }();
  (void)checks_registered;
  ul_host_counters().set_metrics_consumed(metrics_enabled);
#endif

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
                                              baseband_gateway_timestamp            timestamp,
                                              rx_buffer_handle                      owner)
{
  // The handle is kept for the whole call: every symbol assembled and submitted from these samples
  // hands it to the PUxCH, which holds a reference per in-flight transform (see the interface). The
  // member is cleared on the way out so that the last symbol's reference is the only one left - the
  // buffer returns to the radio's pool as soon as that transform is finished.
  current_owner = std::move(owner);
  switch (state) {
    case fsm_states::alignment:
      process_alignment(samples, timestamp);
      break;
    case fsm_states::collecting:
      process_collecting(samples, timestamp);
      break;
  }
  current_owner.reset();
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
  // One host copy of the samples per symbol (the assembly), counted here rather than where the copy
  // runs: a symbol whose samples arrive in two radio blocks is written in two calls.
  ul_host_counters().count_assembled();

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
    ul_host_counters().count_round_trip();
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
  ul_host_counters().count_symbol();
  ul_host_counters().observe(cfo_processor.get_cfo_hz(), cfo_processor.get_nof_scheduled_commands());

  // Advance CFO processor number of samples.
  cfo_processor.advance(symbol_buffer.get_nof_samples());

  // Process symbol by PRACH processor.
  prach_processor_baseband::symbol_context prach_context = {
      .slot = current_slot, .symbol = current_symbol_index, .sector = sector_id};
  prach_proc->get_baseband().process_symbol(symbol_buffer.get_reader(), prach_context);

  // Process symbol by PUxCH processor.
  lower_phy_rx_symbol_context puxch_context = {
      .slot = current_slot, .sector = sector_id, .nof_symbols = current_symbol_index};
  bool processed = puxch_proc->get_baseband().process_symbol(
      symbol_buffer.get_reader(), puxch_context, current_symbol_buffer, current_owner);

  // Baseband metrics. Three passes over every sample of the symbol (average power, peak power and
  // the clipping count), for values that only the application's RU metrics collector reads: with the
  // metrics disabled nothing consumes them, so they are not measured at all and the samples are not
  // read again on the host (see lower_phy_configuration::are_metrics_enabled). `processed` is what
  // the PUxCH processor returns for a symbol it actually took - the metrics describe a received
  // symbol, so they are measured for exactly those.
  if (processed && metrics_enabled) {
    ul_host_counters().count_metrics();
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
