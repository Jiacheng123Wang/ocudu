// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "puxch_processor_impl.h"
#include "ocudu/gateways/baseband/buffer/baseband_gateway_buffer_reader.h"
#include "ocudu/phy/lower/lower_phy_rx_symbol_context.h"
#include "ocudu/phy/support/resource_grid_context.h"
#include "ocudu/phy/support/resource_grid_writer.h"
#include "ocudu/support/executors/ul_pipeline_probe.h"

using namespace ocudu;

bool puxch_processor_impl::process_symbol(const baseband_gateway_buffer_reader& samples,
                                          const lower_phy_rx_symbol_context&    context)
{
  ocudu_assert(notifier != nullptr, "Notifier has not been connected.");

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
    // `pipeline_depth` symbols. The FFTs therefore overlap with the radio, while the grid content of
    // a symbol is still written before that symbol is reported to the upper PHY.
    for (unsigned i_port = 0; i_port != nof_rx_ports; ++i_port) {
      // The slot about to be reused holds the oldest in-flight transform: its command buffer was
      // submitted `pipeline_depth` symbols ago, so finishing it does not stall on the GPU.
      if (nof_in_flight == pipeline_depth) {
        finish_oldest_symbol();
      }
      unsigned slot = next_pipeline_slot % pipeline_depth;
      ++next_pipeline_slot;
      demodulator->submit_symbol(samples.get_channel_buffer(i_port), i_port, symbol_index_subframe, slot);
      in_flight[(in_flight_begin + nof_in_flight) % max_in_flight_symbols] = {.context = context, .slot = slot};
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
    demodulator->demodulate(
        current_grid.get().get_writer(), samples.get_channel_buffer(i_port), i_port, symbol_index_subframe);
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
  const in_flight_symbol& entry = in_flight[in_flight_begin];
  demodulator->finish_symbol(current_grid.get().get_writer(), entry.slot);
  notifier->on_rx_symbol(current_grid, entry.context, true);
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
