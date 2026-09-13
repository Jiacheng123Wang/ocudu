// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/adt/complex.h"
#include "ocudu/adt/span.h"
#include "ocudu/ran/cyclic_prefix.h"

namespace ocudu {

class resource_grid_writer;

/// Setup configuration parameters.
struct ofdm_demodulator_configuration {
  /// Indicates the numerology of the OFDM demodulator.
  unsigned numerology;
  /// Indicates the resource grid bandwidth in resource blocks.
  unsigned bw_rb;
  /// Indicates the DFT size.
  unsigned dft_size;
  /// Cyclic prefix.
  cyclic_prefix cp;
  /// Number of samples to advance the DFT window. The demodulator compensates for the phase shift internally.
  unsigned nof_samples_window_offset;
  /// Scaling factor at the DFT output.
  float scale;
  /// Carrier center frequency in Hertz.
  double center_freq_Hz;
  /// \brief Write the demodulated OFDM symbols into the resource grid from the device.
  ///
  /// Set by the uplink PHY pipeline mode: the fused lane keeps the grid on the device, so the transform output never
  /// travels back to the host. Requires a DFT engine able to write the grid (see dft_processor_grid_write) and a grid
  /// whose storage is device-addressable (see resource_grid_writer::get_device_view()); a symbol that cannot be
  /// written from the device is written from the host instead, with a warning the first time it happens.
  bool device_grid_write = false;
};

/// \brief Describes an OFDM demodulator that demodulates at symbol granularity.
///
/// \remark Performs OFDM demodulation as per TS38.211 Section 5.3.1 OFDM baseband signal generation for all channels
/// except PRACH.
/// \remark In addition to demodulation, it applies phase compensation as per TS38.211 Section 5.4 Modulation and
/// upconversion
class ofdm_symbol_demodulator
{
public:
  /// Default destructor.
  virtual ~ofdm_symbol_demodulator() = default;

  /// \brief Gets a symbol size including cyclic prefix.
  /// \param[in] symbol_index Indicates the symbol index within the subframe.
  /// \return The number of samples for the given symbol index.
  virtual unsigned get_symbol_size(unsigned symbol_index) const = 0;

  /// \brief Sets the center frequency.
  ///
  /// The implementation of this method must be thread safe and the new center frequency takes effect in the next call
  /// to demodulate().
  ///
  /// \param[in] center_frequency_Hz Given center frequency in Hertz.
  virtual void set_center_frequency(double center_frequency_Hz) = 0;

  /// \brief Demodulates an OFDM signal with symbol granularity.
  /// \param[out] grid Provides the output as frequency-domain signal corresponding to one slot.
  /// \param[in] input Provides the time domain modulated signal.
  /// \param[in] port_index Indicates the port index to demodulate.
  /// \param[in] symbol_index Symbol index within the subframe to demodulate.
  /// \note The input size must be equal to the the symbol size including the cyclic prefix.
  /// \note The output size must be consistent with the configured bandwidth.
  virtual void
  demodulate(resource_grid_writer& grid, span<const ci16_t> input, unsigned port_index, unsigned symbol_index) = 0;

  /// \brief Demodulates a batch of consecutive symbols of one port.
  ///
  /// The default implementation demodulates the symbols one by one. Implementations whose DFT
  /// supports batching (see dft_processor::get_max_batch()) override it to execute all the
  /// transforms of the batch in a single dispatch.
  ///
  /// \note The gNB RX path (puxch_processor_impl) demodulates symbol by symbol on purpose: it is
  /// driven by the radio, which delivers one OFDM symbol at a time, and the upper PHY consumes
  /// the resource grid as soon as a symbol is reported. Batching several symbols of one stream
  /// would delay every symbol of the batch to its last symbol, which is a different wait rather
  /// than a saving. Batching pays off for independent streams instead.
  /// \todo Batch the Rx ports of one symbol (they are independent and available together, so the
  ///       batch adds no latency), and batch the same symbol across carriers / sectors once
  ///       several cells are driven from one place. Multiple PUSCH allocations or UEs of one cell
  ///       share the same per-symbol transform and need no batching.
  ///
  /// \param[out] grid       Provides the output as frequency-domain signal corresponding to one slot.
  /// \param[in]  input      Provides the concatenated time domain symbols, each including its cyclic prefix.
  /// \param[in]  port_index Indicates the port index to demodulate.
  /// \param[in]  first_symbol_index Symbol index within the subframe of the first symbol of the batch.
  /// \param[in]  nof_symbols Number of consecutive symbols to demodulate.
  /// \brief Number of symbols that can be demodulated concurrently (1 = no pipeline).
  ///
  /// The pipeline lets the caller submit the transforms of the in-flight symbols and post-process
  /// them with a lag, so the FFTs overlap with the radio while the grid content of a symbol is
  /// still written before that symbol is reported.
  virtual unsigned get_pipeline_depth() const { return 1; }

  /// \brief Fills the transform slot \c slot with one symbol and submits it without waiting.
  ///
  /// Only valid when get_pipeline_depth() > 1. The caller must call finish_symbol() for the same
  /// slot before reusing it.
  ///
  /// \param[in] grid       Grid the symbol belongs to. A DFT engine able to write the grid from the device (see
  ///                       dft_processor_grid_write) uses it here, so that the transform and the grid write share one
  ///                       command buffer; the host path only needs it later, in finish_symbol(). The grid must stay
  ///                       alive until the slot is finished.
  /// \param[in] input      Time domain samples of the symbol, including its cyclic prefix.
  /// \param[in] port_index Port index of the symbol.
  /// \param[in] symbol_index Symbol index within the subframe.
  /// \param[in] slot       Transform slot (see get_pipeline_depth()).
  virtual void submit_symbol(resource_grid_writer& grid,
                             span<const ci16_t>    input,
                             unsigned              port_index,
                             unsigned              symbol_index,
                             unsigned              slot)
  {
    // Without a pipeline (get_pipeline_depth() == 1) the caller uses demodulate() instead.
    (void)grid;
    (void)input;
    (void)port_index;
    (void)symbol_index;
    (void)slot;
  }

  /// \brief Waits for the transform submitted in \c slot and writes its symbol into the grid.
  virtual void finish_symbol(resource_grid_writer& grid, unsigned slot) { (void)grid; (void)slot; }

  virtual void demodulate_batch(resource_grid_writer& grid,
                                span<const ci16_t>    input,
                                unsigned              port_index,
                                unsigned              first_symbol_index,
                                unsigned              nof_symbols)
  {
    for (unsigned i_symbol = 0; i_symbol != nof_symbols; ++i_symbol) {
      unsigned symbol_index = first_symbol_index + i_symbol;
      unsigned symbol_size  = get_symbol_size(symbol_index);
      demodulate(grid, input.first(symbol_size), port_index, symbol_index);
      input = input.last(input.size() - symbol_size);
    }
  }
};

/// \brief Describes an OFDM demodulator with slot granularity.
///
/// \remark Performs OFDM demodulation as per TS38.211 Section 5.3.1 OFDM baseband signal generation for all channels
/// except PRACH.
/// \remark In addition to demodulation, it applies phase compensation as per TS38.211 Section 5.4 Modulation and
/// upconversion.
class ofdm_slot_demodulator
{
public:
  /// Default destructor.
  virtual ~ofdm_slot_demodulator() = default;

  /// \brief Gets a slot size.
  /// \param[in] slot_index Indicates the slot index within the subframe.
  /// \return The number of samples for the given slot index.
  virtual unsigned get_slot_size(unsigned slot_index) const = 0;

  /// \brief Demodulates an OFDM signal with slot granularity.
  /// \param[out] grid Provides the output as frequency-domain signal corresponding to one slot.
  /// \param[in] input Provides the time domain modulated signal source.
  /// \param[in] port_index Indicates the port index to demodulate.
  /// \param[in] slot_index Slot index within the subframe to demodulate.
  /// \note The input size must be must be equal to the slot size including the cyclic prefixes.
  /// \note The output size consistent with the configured bandwidth.
  virtual void
  demodulate(resource_grid_writer& grid, span<const ci16_t> input, unsigned port_index, unsigned slot_index) = 0;
};

} // namespace ocudu
