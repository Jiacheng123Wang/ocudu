// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "phase_compensation_lut.h"
#include "ocudu/phy/generic_functions/dft_processor.h"
#include "ocudu/phy/lower/modulation/ofdm_demodulator.h"
#include <array>
#include <memory>
#include <vector>

namespace ocudu {

/// OFDM demodulator dependencies. Provides the necessary dependency instances.
struct ofdm_demodulator_dependencies {
  /// DFT instance. The ownership is transferred to the demodulator.
  std::unique_ptr<dft_processor> dft;
};

/// Describes a generic OFDM symbol demodulator.
class ofdm_symbol_demodulator_impl : public ofdm_symbol_demodulator
{
  /// Indicates the DFT size.
  unsigned dft_size;
  /// Indicates the resource grid bandwidth in resource elements.
  unsigned rg_size;
  /// Cyclic prefix type.
  cyclic_prefix cp;
  /// DFT window offset.
  unsigned nof_samples_window_offset;
  /// Numerology.
  subcarrier_spacing scs;
  /// Sampling rate in Hz.
  unsigned sampling_rate_Hz;
  /// Scaling factor at the DFT output.
  float scale;
  /// DFT processor.
  std::unique_ptr<dft_processor> dft;
  /// Phase compensation table.
  phase_compensation_lut phase_compensation_table;
  /// Next center frequency in Hertz.
  std::atomic<double> next_center_freq_Hz;
  /// Current center frequency in Hertz.
  double current_center_freq_Hz;
  /// Internal buffer aimed at storing the phase compensated DFT outputs.
  std::vector<cf_t> compensated_output;
  /// DFT window offset phase compensation.
  std::vector<cf_t> window_phase_compensation;

  /// Maximum number of symbols kept in flight by the pipelined path.
  static constexpr unsigned max_pipeline_depth = 8;

  /// Symbol carried by each in-flight DFT slot.
  struct pipeline_entry {
    unsigned port_index   = 0;
    unsigned symbol_index = 0;
    bool     valid        = false;
  };
  std::array<pipeline_entry, max_pipeline_depth> pipeline_slots = {};

public:
  /// \brief Constructs an OFDM symbol demodulator.
  /// \param[in] ofdm_config  Provides generic OFDM configuration parameters.
  /// \param[in] dependencies Provides specific dependencies.
  ofdm_symbol_demodulator_impl(const ofdm_demodulator_configuration& ofdm_config,
                               ofdm_demodulator_dependencies         dependencies);

  /// \brief Gets the resource grid bandwidth in resource elements.
  /// \return The number of resource elements in the grid.
  unsigned get_rg_size() const { return rg_size; }

  /// \brief Gets the offset in samples to the start of (the cyclic prefix of) a given symbol.
  /// \param[in] symbol_index Indicates the symbol index within the subframe.
  /// \param[in] slot_index Slot index within the subframe containing the symbol to demodulate.
  /// \return The number of samples preceding the given symbol.
  unsigned get_cp_offset(unsigned symbol_index, unsigned slot_index) const;

  // See interface for documentation.
  unsigned get_symbol_size(unsigned symbol_index) const override
  {
    return cp.get_length(symbol_index, scs).to_samples(sampling_rate_Hz) + dft_size;
  }

  // See interface for documentation.
  void set_center_frequency(double center_frequency_Hz) override
  {
    next_center_freq_Hz.store(center_frequency_Hz, std::memory_order_relaxed);
  }

  // See interface for documentation.
  void
  demodulate(resource_grid_writer& grid, span<const ci16_t> input, unsigned port_index, unsigned symbol_index) override;

  /// \brief Pipeline depth: how many symbols can be in flight (1 = synchronous per-symbol path).
  ///
  /// The per-symbol DFT of the RX chain costs a command buffer round trip, so the puxch now submits
  /// the transforms without waiting and post-processes them with a lag: the FFTs of the in-flight
  /// symbols overlap with the radio while the grid content of a symbol is still written before that
  /// symbol is reported to the upper PHY. The depth is bounded by the DFT ring capacity and by
  /// \c max_pipeline_depth (a deeper ring delays the symbol reports further).
  unsigned get_pipeline_depth() const override;

  /// \brief Fills DFT slot \c slot with one symbol and submits it without waiting.
  void submit_symbol(span<const ci16_t> input, unsigned port_index, unsigned symbol_index, unsigned slot) override;

  /// \brief Waits for DFT slot \c slot and writes the symbol it carries into the grid.
  void finish_symbol(resource_grid_writer& grid, unsigned slot) override;

  /// \brief Demodulates the symbols of a batch with a single DFT dispatch when the DFT processor
  /// supports batching, otherwise symbol by symbol (see the interface documentation).
  void demodulate_batch(resource_grid_writer& grid,
                        span<const ci16_t>    input,
                        unsigned              port_index,
                        unsigned              first_symbol_index,
                        unsigned              nof_symbols) override;

private:
  /// \brief Converts one symbol of time-domain samples into the DFT input buffer \c dft_input.
  void fill_dft_input(span<cf_t> dft_input, span<const ci16_t> input, unsigned symbol_index);

  /// \brief Applies phase and window compensation to one DFT output and writes it into the grid.
  void process_dft_output(resource_grid_writer& grid,
                          span<const cf_t>      dft_output,
                          unsigned              port_index,
                          unsigned              symbol_index);
};

/// Describes a generic OFDM slot demodulator.
class ofdm_slot_demodulator_impl : public ofdm_slot_demodulator
{
  /// Cyclic prefix type.
  cyclic_prefix cp;
  /// Instance of symbol demodulator.
  std::unique_ptr<ofdm_symbol_demodulator> symbol_demodulator;

public:
  /// \brief Constructs an OFDM slot demodulator.
  /// \param[in] ofdm_config       OFDM factory parameters.
  /// \param[in] symbol_modulator_ OFDM symbol demodulator instance.
  ofdm_slot_demodulator_impl(const ofdm_demodulator_configuration&    ofdm_config,
                             std::unique_ptr<ofdm_symbol_demodulator> symbol_demodulator_) :
    cp(ofdm_config.cp), symbol_demodulator(std::move(symbol_demodulator_))
  {
    ocudu_assert(symbol_demodulator, "Invalid OFDM symbol demodulator.");
  }

  // See interface for documentation;
  unsigned get_slot_size(unsigned slot_index) const override;

  // See interface for documentation;
  void
  demodulate(resource_grid_writer& grid, span<const ci16_t> input, unsigned port_index, unsigned slot_index) override;
};

} // namespace ocudu
