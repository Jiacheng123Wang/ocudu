// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "ofdm_demodulator_impl.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/ocuduvec/conversion.h"
#include "ocudu/ocuduvec/copy.h"
#include "ocudu/ocuduvec/prod.h"
#include "ocudu/ocuduvec/sc_prod.h"
#include "ocudu/ocuduvec/zero.h"
#include "ocudu/phy/support/resource_grid_writer.h"
#include "ocudu/ran/subcarrier_spacing.h"
#include "ocudu/support/error_handling.h"
#include <algorithm>
#include <cstdlib>

using namespace ocudu;

ofdm_symbol_demodulator_impl::ofdm_symbol_demodulator_impl(const ofdm_demodulator_configuration& ofdm_config,
                                                           ofdm_demodulator_dependencies         dependencies) :
  dft_size(ofdm_config.dft_size),
  rg_size(ofdm_config.bw_rb * NOF_SUBCARRIERS_PER_RB),
  cp(ofdm_config.cp),
  nof_samples_window_offset(ofdm_config.nof_samples_window_offset),
  scs(to_subcarrier_spacing(ofdm_config.numerology)),
  sampling_rate_Hz(to_sampling_rate_Hz(scs, dft_size)),
  scale(ofdm_config.scale),
  dft(std::move(dependencies.dft)),
  phase_compensation_table(to_subcarrier_spacing(ofdm_config.numerology),
                           ofdm_config.cp,
                           ofdm_config.dft_size,
                           ofdm_config.center_freq_Hz,
                           false),
  next_center_freq_Hz(ofdm_config.center_freq_Hz),
  current_center_freq_Hz(ofdm_config.center_freq_Hz)
{
  report_fatal_error_if_not(std::isnormal(scale), "Invalid scaling factor {}.", scale);
  report_fatal_error_if_not(
      dft_size > rg_size, "The DFT size ({}) must be greater than the resource grid size ({}).", dft_size, rg_size);

  // Fill DFT input with zeros.
  ocuduvec::zero(dft->get_input());

  // Set the right size to the internal phase compensation buffer.
  compensated_output.resize(dft_size);

  if (ofdm_config.nof_samples_window_offset != 0) {
    // Verify the window is valid.
    ocudu_assert(ofdm_config.nof_samples_window_offset < (144 * ofdm_config.dft_size) / 2048,
                 "The DFT window offset (i.e., {}) must be lower than {}.",
                 ofdm_config.nof_samples_window_offset,
                 (144 * ofdm_config.dft_size) / 2048);

    // Prepare phase compensation vector.
    window_phase_compensation.resize(dft_size);

    // Discrete frequency of the complex exponential.
    float omega = static_cast<float>(ofdm_config.nof_samples_window_offset) * static_cast<float>(2.0 * M_PI) /
                  static_cast<float>(dft_size);
    for (unsigned i = 0; i != dft_size; ++i) {
      window_phase_compensation[i] = std::polar(1.0F, omega * static_cast<float>(i));
    }
  }

  // Device grid write: the engine that can do it writes both the transform and the grid in one command buffer, so the
  // transform output never travels back to the host. The table is constant, so it is published once.
  device_grid_write        = ofdm_config.device_grid_write;
  grid_consumed_on_device  = ofdm_config.grid_consumed_on_device;
  if (device_grid_write) {
    grid_write = dft->get_grid_write();
    if (grid_write == nullptr) {
      ocudulog::fetch_basic_logger("PHY").warning(
          "OFDM demodulator: the device grid write was requested but the DFT engine does not provide it; the grid will "
          "be written from the host");
      device_grid_write = false;
    } else if (!grid_write->set_grid_write_window(window_phase_compensation)) {
      ocudulog::fetch_basic_logger("PHY").warning(
          "OFDM demodulator: publishing the DFT window table failed; the grid will be written from the host");
      device_grid_write = false;
      grid_write         = nullptr;
    }
  }
}

bool ofdm_symbol_demodulator_impl::submit_grid_write(resource_grid_writer& grid,
                                                     unsigned              port_index,
                                                     unsigned              symbol_index,
                                                     unsigned              slot,
                                                     span<const ci16_t>    time_input)
{
  if (!device_grid_write || (grid_write == nullptr)) {
    return false;
  }

  const resource_grid_device_view view = grid.get_device_view();
  if (!grid_write->supports_grid_write(view) || (view.nof_subc != rg_size)) {
    // The grid cannot be written from the device (its storage is not device-addressable, or it does not match this
    // demodulator). Report it once: falling back silently would hide a configuration mistake for the whole run.
    if (!device_grid_write_failed) {
      device_grid_write_failed = true;
      ocudulog::fetch_basic_logger("PHY").warning(
          "OFDM demodulator: the resource grid cannot be written from the device (view valid={}, subcarriers={} vs "
          "{}); falling back to the host grid write",
          view.is_valid(),
          view.nof_subc,
          rg_size);
    }
    return false;
  }

  // The phase compensation table may have been rebuilt by fill_dft_input() (center frequency change), so the
  // coefficient is taken here, after the input was filled.
  dft_grid_write_params params;
  params.view         = view;
  params.port         = port_index;
  params.symbol       = symbol_index % get_nsymb_per_slot(cp);
  params.nof_subc     = rg_size;
  params.map_offset   = dft_size - rg_size / 2;
  params.coefficient  = phase_compensation_table.get_coefficient(symbol_index) * scale;
  params.apply_window = !window_phase_compensation.empty();
  if (!time_input.empty()) {
    // The transform reads the radio's int16 samples instead of the engine's float2 ring: the cyclic
    // prefix is skipped by the offset and the per-component scaling is the one the host would apply
    // (ocuduvec::convert with ocuduvec::scaling_factor_ci16_to_cf). The engine refuses the request,
    // and the caller stages the input, when the samples are not in a registered page-aligned
    // allocation or the symbol does not fit in it.
    params.time_samples      = time_input.data();
    params.time_samples_bytes = time_input.size() * sizeof(ci16_t);
    // The symbol's samples start at the cyclic prefix, and the transform reads from there minus the
    // DFT window offset - exactly the slice fill_dft_input() converts (see its subspan).
    params.time_window_start = cp.get_length(symbol_index, scs).to_samples(sampling_rate_Hz) - nof_samples_window_offset;
    params.time_gain          = 1.0F / ocuduvec::scaling_factor_ci16_to_cf;
  }
  return grid_write->submit_grid_write(slot, params);
}

unsigned ofdm_symbol_demodulator_impl::get_cp_offset(unsigned symbol_index, unsigned slot_index) const
{
  // Calculate number of symbols per slot.
  unsigned nsymb = get_nsymb_per_slot(cp);

  // Calculate the offset in samples to the start of the symbol CP within the current slot
  unsigned cp_offset = 0;
  for (unsigned symb_idx = 0; symb_idx != symbol_index; ++symb_idx) {
    cp_offset += cp.get_length(nsymb * slot_index + symb_idx, scs).to_samples(sampling_rate_Hz) + dft_size;
  }

  return cp_offset;
}

void ofdm_symbol_demodulator_impl::refresh_phase_compensation()
{
  // Recalculate phase compensation if the center frequency has changed.
  double center_freq_Hz = next_center_freq_Hz.load(std::memory_order::memory_order_relaxed);
  if (center_freq_Hz != current_center_freq_Hz) {
    phase_compensation_table = phase_compensation_lut(scs, cp, dft_size, center_freq_Hz, false);
    current_center_freq_Hz   = center_freq_Hz;
  }
}

void ofdm_symbol_demodulator_impl::fill_dft_input(span<cf_t>         dft_input,
                                                  span<const ci16_t>  input,
                                                  unsigned            symbol_index)
{
  refresh_phase_compensation();

  // Calculate cyclic prefix length.
  unsigned cp_len = cp.get_length(symbol_index, scs).to_samples(sampling_rate_Hz);

  // Make sure output buffer matches the symbol size.
  ocudu_assert(input.size() == (cp_len + dft_size),
               "The input buffer size ({}) does not match the symbol index {} size ({}+{}={}). SCS={}kHz.",
               input.size(),
               symbol_index,
               cp_len,
               cp_len + dft_size,
               scs_to_khz(scs));

  // Prepare the DFT inputs, while skipping the cyclic prefix.
  ocuduvec::convert(dft_input,
                    input.subspan(cp_len - nof_samples_window_offset, dft_size),
                    ocuduvec::scaling_factor_ci16_to_cf);
}

void ofdm_symbol_demodulator_impl::process_dft_output(resource_grid_writer& grid,
                                                      span<const cf_t>      dft_output,
                                                      unsigned              port_index,
                                                      unsigned              symbol_index)
{
  // Calculate number of symbols per slot.
  unsigned nsymb = get_nsymb_per_slot(cp);

  // Get phase correction (TS138.211, Section 5.4)
  cf_t phase_compensation = phase_compensation_table.get_coefficient(symbol_index);

  // Apply scaling and phase compensation.
  ocuduvec::sc_prod(compensated_output, dft_output, phase_compensation * scale);

  // Compensate DFT window offset phase shift.
  if (!window_phase_compensation.empty()) {
    ocuduvec::prod(compensated_output, window_phase_compensation, compensated_output);
  }

  // Map the upper bound frequency domain data.
  span<cf_t> upper_bound(&compensated_output[dft_size - rg_size / 2], rg_size / 2);
  grid.put(port_index, symbol_index % nsymb, 0, upper_bound);

  // Map the lower bound frequency domain data.
  span<cf_t> lower_bound(&compensated_output[0], rg_size / 2);
  grid.put(port_index, symbol_index % nsymb, rg_size / 2, lower_bound);
}

void ofdm_symbol_demodulator_impl::demodulate(resource_grid_writer& grid,
                                              span<const ci16_t>    input,
                                              unsigned              port_index,
                                              unsigned              symbol_index)
{
  // Fill the DFT input, execute one transform and post-process its output.
  fill_dft_input(dft->get_input().first(dft_size), input, symbol_index);
  span<const cf_t> dft_output = dft->run();
  process_dft_output(grid, dft_output, port_index, symbol_index);
}

// NOTE: the gNB RX path (puxch_processor_impl) calls demodulate() once per OFDM symbol because the
// radio paces the processing symbol by symbol, so the batched path below is dormant there. It pays
// off only for *independent* transform streams: the Rx ports of one symbol (available together, so
// the batch adds no latency) or the same symbol across carriers/sectors. Multiple PUSCH
// allocations or UEs of one cell share one per-symbol transform and need no batching.
// TODO(multi-port/multi-carrier): batch the ports of one symbol first (no added latency), then the
// same symbol of several carriers once multiple cells are driven from one place.
unsigned ofdm_symbol_demodulator_impl::get_pipeline_depth() const
{
  // Debug probe (documented in the plan): OCUDU_DFT_PIPELINE_DEPTH overrides the depth so a
  // suspicious RX regression can be bisected between the pipelined path (depth > 1) and the
  // original synchronous per-symbol path (depth = 1) without rebuilding.
  static const unsigned debug_depth = []() {
    const char* env = std::getenv("OCUDU_DFT_PIPELINE_DEPTH");
    return (env != nullptr) ? static_cast<unsigned>(std::strtoul(env, nullptr, 10)) : max_pipeline_depth;
  }();

  // Only as deep as the DFT ring and the configured bound; 1 keeps the synchronous per-symbol path.
  return std::min({dft->get_max_batch(), max_pipeline_depth, std::max(1U, debug_depth)});
}

void ofdm_symbol_demodulator_impl::submit_symbol(resource_grid_writer& grid,
                                                 span<const ci16_t>    input,
                                                 unsigned              port_index,
                                                 unsigned              symbol_index,
                                                 unsigned              slot)
{
  ocudu_assert(slot < max_pipeline_depth, "Invalid pipeline slot {}.", slot);

  // The transform input, and the grid write that rides the same dispatch: the engine reads the
  // radio's int16 samples straight out of the buffer the upper layers filled (see
  // dft_grid_write_params::time_samples), so NOTHING on the host touches the samples - the cyclic
  // prefix is an offset and the int16 -> float scaling is one multiply in the kernel. When the
  // engine refuses (the samples are not in a page-aligned allocation, or the symbol does not fit),
  // the input is staged on the host as before, once per run: refresh_phase_compensation() keeps the
  // per-symbol coefficient below correct whether or not fill_dft_input() ran.
  refresh_phase_compensation();
  bool device_write = false;
  if (!time_input_failed && device_grid_write && (grid_write != nullptr)) {
    device_write = submit_grid_write(grid, port_index, symbol_index, slot, input);
    if (!device_write) {
      time_input_failed = true;
      ocudulog::fetch_basic_logger("PHY").warning(
          "OFDM demodulator: the transform input cannot be read from the radio buffer; the samples are staged on the "
          "host for this run");
    }
  }

  // The device grid write (when available) has to be encoded together with the transform, so it happens here and not in
  // finish_symbol(): the transform output never has to reach the host.
  if (!device_write) {
    fill_dft_input(dft->get_input().subspan(static_cast<size_t>(slot) * dft_size, dft_size), input, symbol_index);
    device_write = submit_grid_write(grid, port_index, symbol_index, slot);
  }
  if (!device_write) {
    dft->run_async(slot);
  }
  pipeline_slots[slot] = {
      .port_index = port_index, .symbol_index = symbol_index, .valid = true, .device_write = device_write};
}

/// \brief Whether a configuration exists whose readers touch the resource grid on the HOST.
///
/// The front-end fence (see finish_symbol()) only orders the grid's producer against readers on the
/// DEVICE, so these are the routes that must keep the host wait: the host LS pre-stage and the CPU
/// estimator read the pilots out of the grid, the debug capture dumps it, and OCUDU_EQ_GATHER=0 makes
/// the equalizer gather the received symbols on the host instead of on the device.
/// Read once: a leg sets them before the process starts.
static bool host_grid_readers_enabled()
{
  static const bool on = (std::getenv("OCUDU_CE_CPU_LS") != nullptr) || (std::getenv("OCUDU_CE_CPU_CE") != nullptr) ||
                         (std::getenv("OCUDU_UL_DUMP") != nullptr) || (std::getenv("OCUDU_EQ_GATHER") != nullptr);
  return on;
}

/// \brief Whether the front-end fence orders the front-end DFTs against the back-end grid readers.
static bool front_end_fence_enabled()
{
  // Same knob as shared_queue's fence (OCUDU_UL_FRONTEND_FENCE): one switch for BOTH ends of the
  // relation, because a fence that is signalled but never waited on - or the other way round - is not a
  // half-optimization, it is an unordered chain.
  const char* env = std::getenv("OCUDU_UL_FRONTEND_FENCE");
  return (env != nullptr) && (std::strtoul(env, nullptr, 10) != 0);
}

void ofdm_symbol_demodulator_impl::finish_symbol(resource_grid_writer& grid, unsigned slot)
{
  ocudu_assert(slot < max_pipeline_depth, "Invalid pipeline slot {}.", slot);
  ocudu_assert(pipeline_slots[slot].valid, "Pipeline slot {} holds no symbol.", slot);

  // The upper PHY reads the grid as soon as the symbol is reported, and with the device write that
  // reads memory the GPU produced - but it reads it on the DEVICE here: the estimator extracts the
  // pilots with resource_grid_reader::get_device_view() and the equalizer gathers the received symbols
  // with set_device_grid(). With the front-end fence on, that ordering is established by the shared
  // event instead (see shared_queue::front_end_wait()), so the host does not have to wait for each
  // symbol - which is exactly the per-symbol synchronization the fusion removes (design document,
  // 48.189).
  //
  // The wait is KEPT whenever the grid can also be read on the host: device_grid_write false means the
  // grid is not device-resident at all, and host_grid_readers_enabled() lists the routes whose readers
  // touch it on the host. Keeping it costs the optimization and is always correct; skipping it where a
  // host reader exists would read memory the GPU has not written yet.
  // ... and only when the deployment DECLARED that the grid's consumers read it on the device: writing it
  // there (device_grid_write) says nothing about who reads it, and a host reader - the demodulator's own
  // test verifies the grid on the host - would read memory the GPU has not written yet.
  const bool fence_orders_the_grid =
      front_end_fence_enabled() && device_grid_write && grid_consumed_on_device && !host_grid_readers_enabled();
  if (!fence_orders_the_grid) {
    dft->wait_slot(slot);
  } else {
    // The host wait for this symbol is gone: the back end is ordered by the fence event instead. Said
    // once, because it is the property the whole configuration now depends on - a host consumer that
    // reads the grid without being listed in host_grid_readers_enabled() would read memory the GPU has
    // not written yet, and the leg would only see it as wrong LLRs. The counters that prove it are the
    // ones the contract prints (ce device estimates, equalizer ch_re): both must report host=0.
    static bool reported = false;
    if (!reported) {
      reported = true;
      ocudulog::fetch_basic_logger("PHY").info(
          "OFDM demodulator: the resource grid is handed to the back end through the front-end fence "
          "(no host wait per symbol). Its consumers must read it on the device - see the pipeline contract");
    }
  }

  if (!pipeline_slots[slot].device_write) {
    span<const cf_t> dft_output = dft->get_output_batch().subspan(static_cast<size_t>(slot) * dft_size, dft_size);
    process_dft_output(grid, dft_output, pipeline_slots[slot].port_index, pipeline_slots[slot].symbol_index);
  }
  pipeline_slots[slot].valid = false;
}

void ofdm_symbol_demodulator_impl::demodulate_batch(resource_grid_writer& grid,
                                                    span<const ci16_t>    input,
                                                    unsigned              port_index,
                                                    unsigned              first_symbol_index,
                                                    unsigned              nof_symbols)
{
  // Batched execution requires a DFT processor able to run several transforms in one dispatch.
  if ((nof_symbols <= 1) || (dft->get_max_batch() < nof_symbols)) {
    ofdm_symbol_demodulator::demodulate_batch(grid, input, port_index, first_symbol_index, nof_symbols);
    return;
  }

  // 1) Fill the input of every transform of the batch.
  span<cf_t>         batch_input = dft->get_input().first(static_cast<size_t>(nof_symbols) * dft_size);
  span<const ci16_t> remaining   = input;
  for (unsigned i_symbol = 0; i_symbol != nof_symbols; ++i_symbol) {
    unsigned symbol_index = first_symbol_index + i_symbol;
    unsigned symbol_size  = get_symbol_size(symbol_index);
    fill_dft_input(batch_input.subspan(static_cast<size_t>(i_symbol) * dft_size, dft_size),
                   remaining.first(symbol_size),
                   symbol_index);
    remaining = remaining.last(remaining.size() - symbol_size);
  }

  // 2) Execute all transforms with a single dispatch.
  span<const cf_t> batch_output = dft->run_batch(nof_symbols);

  // 3) Post-process each transform output.
  for (unsigned i_symbol = 0; i_symbol != nof_symbols; ++i_symbol) {
    process_dft_output(grid,
                       batch_output.subspan(static_cast<size_t>(i_symbol) * dft_size, dft_size),
                       port_index,
                       first_symbol_index + i_symbol);
  }
}

unsigned ofdm_slot_demodulator_impl::get_slot_size(unsigned slot_index) const
{
  unsigned nsymb = get_nsymb_per_slot(cp);
  unsigned count = 0;

  // Iterate all symbols of the slot and accumulate
  for (unsigned symbol_idx = 0; symbol_idx != nsymb; ++symbol_idx) {
    count += symbol_demodulator->get_symbol_size(nsymb * slot_index + symbol_idx);
  }

  return count;
}

void ofdm_slot_demodulator_impl::demodulate(resource_grid_writer& grid,
                                            span<const ci16_t>    input,
                                            unsigned              port_index,
                                            unsigned              slot_index)
{
  unsigned nsymb = get_nsymb_per_slot(cp);

  // Demodulate the whole slot: implementations whose DFT supports batching execute all the
  // symbol transforms in a single dispatch.
  symbol_demodulator->demodulate_batch(grid, input, port_index, nsymb * slot_index, nsymb);
}
