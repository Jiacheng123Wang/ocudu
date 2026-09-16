// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "puxch_processor_impl.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>
#include "ocudu/gateways/baseband/buffer/baseband_gateway_buffer_reader.h"
#include "ocudu/phy/lower/lower_phy_rx_symbol_context.h"
#include "ocudu/phy/support/resource_grid_context.h"
#include "ocudu/phy/support/resource_grid_writer.h"
#include "ocudu/support/executors/ul_pipeline_probe.h"

using namespace ocudu;


namespace {

/// \brief Debug capture of the time-domain samples handed to the DFT (OCUDU_UL_DUMP_TD=<prefix>).
///
/// Writes <prefix>_td.txt (one line per submitted transform: slot, symbol, port, size) and
/// <prefix>_td.bin (the ci16 samples back to back, in the same order), for the first
/// OCUDU_UL_DUMP_TD_SLOTS (default 4) slots that carry a transmission.  A resource grid capture
/// (OCUDU_UL_DUMP) is taken after the DFT, so only this one can replay the transform itself - which
/// is what separates the pipelined front end from the serial one.
class td_capture
{
public:
  static bool enabled()
  {
    static const bool on = (std::getenv("OCUDU_UL_DUMP_TD") != nullptr);
    return on;
  }

  static void capture(slot_point slot, unsigned symbol_index, unsigned port, span<const ci16_t> samples)
  {
    if (!enabled()) {
      return;
    }
    static const unsigned max_slots = []() {
      const char* env = std::getenv("OCUDU_UL_DUMP_TD_SLOTS");
      return (env != nullptr) ? static_cast<unsigned>(std::strtoul(env, nullptr, 10)) : 4U;
    }();
    static std::set<unsigned> captured_slots;
    static std::mutex         mutex;

    std::lock_guard lock(mutex);
    if (captured_slots.count(slot.count()) == 0) {
      if (captured_slots.size() >= max_slots) {
        return;
      }
      captured_slots.insert(slot.count());
    }

    const std::string prefix = std::getenv("OCUDU_UL_DUMP_TD");
    if (FILE* f = std::fopen((prefix + "_td.txt").c_str(), "a")) {
      std::fprintf(f, "slot=%u symbol=%u port=%u size=%zu\n", slot.count(), symbol_index, port, samples.size());
      std::fclose(f);
    }
    if (FILE* f = std::fopen((prefix + "_td.bin").c_str(), "ab")) {
      std::fwrite(samples.data(), sizeof(ci16_t), samples.size(), f);
      std::fclose(f);
    }
  }
};

} // namespace

unsigned puxch_processor_impl::acquire_symbol_buffer()
{
  // Hand out a buffer no transform reads. A buffer is in use from the moment the symbol assembled in
  // it is handed over (process_symbol()) until the transform of its last port is finished, and a
  // symbol that submits nothing - no grid for its slot - never marks it. Waiting here, instead of
  // overwriting, is what keeps the samples of an in-flight symbol valid: it is also why the caller
  // needs no per-transform copy of them.
  while (buffer_in_use[next_symbol_buffer]) {
    finish_oldest_symbol();
  }

  unsigned buffer      = next_symbol_buffer;
  next_symbol_buffer   = (buffer + 1) % nof_symbol_buffers;
  last_acquired_buffer = buffer;
  return buffer;
}

bool puxch_processor_impl::process_symbol(const baseband_gateway_buffer_reader& samples,
                                          const lower_phy_rx_symbol_context&    context,
                                          std::optional<unsigned>               buffer_index,
                                          uplink_processor_baseband::rx_buffer_handle owner)
{
  ocudu_assert(notifier != nullptr, "Notifier has not been connected.");
  if (buffer_index.has_value()) {
    ocudu_assert(*buffer_index < nof_symbol_buffers, "Invalid symbol buffer {}.", *buffer_index);
    // The caller writes the symbol into the buffer it acquired, and only then hands it over: taking the
    // samples from anywhere else would mean overwriting a buffer a transform may still be reading.
    ocudu_assert(*buffer_index == last_acquired_buffer,
                 "Symbol assembled in buffer {} but buffer {} was acquired.",
                 *buffer_index,
                 last_acquired_buffer);
  }
  // Without a symbol buffer the samples are a slice of the receive buffer the caller is processing: no
  // buffer of this processor holds them, so there is nothing to release here - only the receive buffer
  // handle (owner), which every transform of this symbol takes a reference to.

  // Check if the slot has changed.
  if (context.slot != current_slot) {
    // Finish the symbols of the previous slot that are still in flight (normally none: the slot is
    // drained when its last symbol is processed).
    drain_pipeline();

    // Update slot.
    current_slot = context.slot;

    // Exchange an empty request with the current slot with a stored request.
    auto request = requests.exchange({context.slot, shared_resource_grid()});

    // Handle the returned request.
    if (!request.resource) {
      // If the request resource grid pointer is invalid, the request is empty.
      current_grid.release();
    } else if (current_slot != request.slot) {
      // If the slot of the request does not match the current slot, then notify a late event.
      resource_grid_context late_context = {.slot = request.slot, .sector = context.sector};
      notifier->on_puxch_request_late(late_context);
      current_grid.release();
    } else {
      // If the request is valid, then select request grid.
      current_grid = std::move(request.resource);
    }
  }

  // Skip symbol processing if the context slot does not match with the current slot or no resource grid is available.
  if (!current_grid) {
    return false;
  }

  // Symbol index within the subframe.
  unsigned symbol_index_subframe = context.nof_symbols + context.slot.subframe_slot_index() * nof_symbols_per_slot;

  // Report the resource elements of each port.
  unsigned pipeline_depth = demodulator->get_pipeline_depth();

  if (pipeline_depth > 1) {
    // Pipelined path: submit the per-symbol DFTs without waiting and post-process them with a lag of
    // `pipeline_depth` transforms (there is one transform per receive port, so with several ports the
    // lag in symbols is `pipeline_depth / nof_rx_ports`). The FFTs therefore overlap with the radio,
    // while the grid content of a symbol is still written before that symbol is reported.
    // The buffer holding this symbol (when the caller assembled it in one) cannot be handed out again
    // while any of its transforms is in flight, so it is marked as in use here and released when the
    // last port is finished.
    if (buffer_index.has_value()) {
      buffer_in_use[*buffer_index] = true;
    }
    for (unsigned i_port = 0; i_port != nof_rx_ports; ++i_port) {
      // Safety net: acquire_symbol_buffer() already made room for the transforms of this symbol. The
      // slot about to be reused holds the oldest in-flight transform - its command buffer was
      // submitted `pipeline_depth` transforms ago - so finishing it does not stall on the GPU.
      if (nof_in_flight == pipeline_depth) {
        finish_oldest_symbol();
      }
      unsigned slot = next_pipeline_slot % pipeline_depth;
      ++next_pipeline_slot;
      span<const ci16_t> td_samples = samples.get_channel_buffer(i_port);
      td_capture::capture(context.slot, symbol_index_subframe, i_port, td_samples);
      demodulator->submit_symbol(current_grid.get().get_writer(), td_samples, i_port, symbol_index_subframe, slot);
      in_flight[(in_flight_begin + nof_in_flight) % max_in_flight_symbols] = {
          .context      = context,
          .slot         = slot,
          .buffer_index = buffer_index,
          // One reference per transform: the samples stay alive until the port that reads them has
          // been finished, whichever port is the last one (see finish_oldest_symbol()).
          .owner     = owner,
          .last_port = (i_port + 1 == nof_rx_ports)};
      ++nof_in_flight;
    }

    // The whole slot has been submitted: finish the remaining symbols before the grid is released,
    // so the time-frequency phase closes only once every symbol of the slot is in the grid.
    if (context.nof_symbols == nof_symbols_per_slot - 1) {
      drain_pipeline();
      ul_pipeline_probe::get().record_t2f_end(context.slot.count());
      current_grid.release();
    }

    return true;
  }

  // Demodulate each of the ports.
  for (unsigned i_port = 0; i_port != nof_rx_ports; ++i_port) {
    span<const ci16_t> td_samples = samples.get_channel_buffer(i_port);
    td_capture::capture(context.slot, symbol_index_subframe, i_port, td_samples);
    demodulator->demodulate(current_grid.get().get_writer(), td_samples, i_port, symbol_index_subframe);
  }

  // Notify.
  notifier->on_rx_symbol(current_grid, context, true);

  // Release current grid if the slot is completed.
  if (context.nof_symbols == nof_symbols_per_slot - 1) {
    // The whole-slot OFDM demodulation (FFT) has just finished: the frequency-domain symbols of the slot are
    // ready for the upper PHY. End timestamp of the time-frequency phase segment.
    ul_pipeline_probe::get().record_t2f_end(context.slot.count());
    current_grid.release();
  }

  return true;
}

void puxch_processor_impl::finish_oldest_symbol()
{
  ocudu_assert(nof_in_flight != 0, "No in-flight symbol to finish.");
  // The transforms in flight were submitted for the grid currently held: it is only released once
  // they have all been finished.
  ocudu_assert(static_cast<bool>(current_grid), "The in-flight transforms belong to no resource grid.");
  const in_flight_symbol& entry = in_flight[in_flight_begin];
  demodulator->finish_symbol(current_grid.get().get_writer(), entry.slot);
  // Only the last port of a symbol completes it: the upper PHY must not be told that a symbol is
  // ready while another of its ports is still missing from the grid.
  if (entry.last_port) {
    // No transform reads the buffer of the symbol anymore: the caller may assemble a new symbol in it.
    // When the samples were never assembled - they were read where the radio put them - there is no
    // buffer to release and the receive buffer handle below is the only lifetime that mattered.
    if (entry.buffer_index.has_value()) {
      buffer_in_use[*entry.buffer_index] = false;
    }
    notifier->on_rx_symbol(current_grid, entry.context, true);
  }
  // The reference to the samples is dropped with the entry - EXPLICITLY, because the ring slot keeps
  // the old entry until it is reused, and a handle living that long would starve the radio's pool
  // (four buffers: the receive loop would block on the fifth). This is the whole lifetime contract of
  // a receive buffer (see rx_buffer_handle); the guard test in puxch_processor_test is what caught it
  // being left to the ring's reuse, which no offline gate that stops above the radio can see.
  // Retire the entry: its samples belong to the radio again from here on.
  in_flight[in_flight_begin].owner.reset();
  in_flight_begin = (in_flight_begin + 1) % max_in_flight_symbols;
  --nof_in_flight;
}

void puxch_processor_impl::drain_pipeline()
{
  while (nof_in_flight != 0) {
    finish_oldest_symbol();
  }
}

void puxch_processor_impl::handle_request(const shared_resource_grid& grid, const resource_grid_context& context)
{
  // Ignore request if the processor has stopped.
  if (stopped.load(std::memory_order_relaxed)) {
    return;
  }

  ocudu_assert(notifier != nullptr, "Notifier has not been connected.");

  // Swap the new request by the current request in the circular array.
  auto request = requests.exchange({context.slot, grid.copy()});

  // If there was a request at the same request index, notify a late event with the context of the discarded request.
  if (request.resource) {
    resource_grid_context late_context = {.slot = request.slot, .sector = context.sector};
    notifier->on_puxch_request_late(late_context);
  }
}

lower_phy_center_freq_controller& puxch_processor_impl::get_center_freq_control()
{
  return *this;
}

bool puxch_processor_impl::set_carrier_center_frequency(double carrier_center_frequency_Hz)
{
  demodulator->set_center_frequency(carrier_center_frequency_Hz);
  return true;
}
