// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "../baseband_cfo_processor.h"
#include "ocudu/adt/tensor.h"
#include "ocudu/gateways/baseband/buffer/baseband_gateway_buffer_dynamic.h"
#include "ocudu/phy/lower/processors/lower_phy_center_freq_controller.h"
#include "ocudu/phy/lower/processors/uplink/prach/prach_processor.h"
#include "ocudu/phy/lower/processors/uplink/puxch/puxch_processor.h"
#include "ocudu/phy/lower/processors/uplink/uplink_processor.h"
#include "ocudu/phy/lower/processors/uplink/uplink_processor_baseband.h"
#include "ocudu/phy/lower/sampling_rate.h"
#include "ocudu/ran/cyclic_prefix.h"
#include "ocudu/ran/slot_point.h"
#include "ocudu/support/ocudu_assert.h"
#include <memory>
#include <optional>

namespace ocudu {

/// Implements a software generic lower PHY uplink processor.
class lower_phy_uplink_processor_impl : public lower_phy_uplink_processor, private uplink_processor_baseband
{
public:
  /// Configuration parameters.
  struct configuration {
    /// Sector identifier.
    unsigned sector_id;
    /// Subcarrier spacing.
    subcarrier_spacing scs;
    /// Cyclic prefix configuration.
    cyclic_prefix cp;
    /// Baseband sampling rate.
    sampling_rate rate;
    /// Number of receive ports.
    unsigned nof_rx_ports;
    /// Measure the baseband metrics of every symbol (see uplink_processor_configuration).
    bool metrics_enabled = true;
  };

  /// \brief Constructs a software generic lower PHY uplink processor that can process PRACH and PUxCH.
  /// \param[in] prach_proc_ PRACH processor.
  /// \param[in] puxch_proc_ PUxCH processor.
  /// \param[in] config      Uplink processor configuration.
  lower_phy_uplink_processor_impl(std::unique_ptr<prach_processor> prach_proc_,
                                  std::unique_ptr<puxch_processor> puxch_proc_,
                                  const configuration&             config);

  // See interface for documentation.
  void connect(uplink_processor_notifier& notifier,
               prach_processor_notifier&  prach_notifier,
               puxch_processor_notifier&  puxch_notifier) override;

  // See interface for documentation.
  void stop() override
  {
    prach_proc->stop();
    puxch_proc->stop();
  }

  // See interface for documentation.
  prach_processor_request_handler& get_prach_request_handler() override;

  // See interface for documentation.
  puxch_processor_request_handler& get_puxch_request_handler() override;

  // See interface for documentation.
  uplink_processor_baseband& get_baseband() override;

  // See interface for documentation.
  baseband_cfo_processor& get_cfo_control() override;

  // See interface for documentation.
  lower_phy_center_freq_controller& get_carrier_center_frequency_control() override;

private:
  /// States.
  enum class fsm_states {
    /// The processor is waiting to receive the next subframe boundary.
    alignment,
    /// \brief The processor consumed a whole OFDM symbol and the next input block starts at a symbol
    /// boundary: the next symbol is chosen from the timestamp of that block.
    ///
    /// This state is what lets a symbol be read where the radio put it: the samples of a symbol are
    /// only looked at when they are in the block being processed, instead of reserving an assembly
    /// buffer for a symbol whose samples have not arrived yet (see process_symbol_boundary()).
    symbol_start,
    /// The processor baseband buffering is synchronized and it is collecting samples.
    collecting
  };

  // See interface for documentation.
  void
  process(const baseband_gateway_buffer_reader& samples, baseband_gateway_timestamp timestamp, rx_buffer_handle owner) override;

  /// \brief Processes samples in alignment state.
  /// \param[in] samples   Input baseband samples.
  /// \param[in] timestamp Time instant in which the first sample within \c samples was received.
  void process_alignment(const baseband_gateway_buffer_reader& samples, baseband_gateway_timestamp timestamp);

  /// \brief Chooses the symbol that starts at \c timestamp and processes its samples.
  ///
  /// The samples of the symbol are handed over where they are - as a slice of \c samples - whenever
  /// they lie entirely in it and they need no modification on the host; otherwise the symbol is
  /// assembled in a symbol buffer (see process_collecting()).
  /// \param[in] samples   Input baseband samples.
  /// \param[in] timestamp Time instant in which the first sample within \c samples was received.
  void process_symbol_boundary(const baseband_gateway_buffer_reader& samples, baseband_gateway_timestamp timestamp);

  /// \brief Collects symbol samples.
  /// \param[in] samples   Input baseband samples.
  /// \param[in] timestamp Time instant in which the first sample within \c samples was received.
  void process_collecting(const baseband_gateway_buffer_reader& samples, baseband_gateway_timestamp timestamp);

  /// \brief Processes a complete OFDM symbol: compensation bookkeeping, PRACH, PUxCH and metrics.
  ///
  /// Called with the samples of exactly one symbol, either a slice of the receive buffer being processed
  /// or the symbol buffer it was assembled in.
  /// \param[in] symbol_samples Samples of the symbol, \c current_symbol_size of them.
  /// \param[in] symbol_buffer  Symbol buffer holding them, or \c std::nullopt when they were read where
  ///                           the radio put them.
  void process_complete_symbol(const baseband_gateway_buffer_reader& symbol_samples,
                               std::optional<unsigned>               symbol_buffer);

  /// Finite state machine state.
  fsm_states state = fsm_states::alignment;
  /// Sector identifier.
  unsigned sector_id;
  /// Subcarrier spacing.
  subcarrier_spacing scs;
  /// Number of receive ports.
  unsigned nof_rx_ports;
  /// Measure the baseband metrics of every symbol (see uplink_processor_configuration).
  bool metrics_enabled = true;
  /// Number of slots per subframe.
  unsigned nof_slots_per_subframe;
  /// Number of symbols per slot.
  unsigned nof_symbols_per_slot;
  /// Number of samples per subframe.
  unsigned nof_samples_per_subframe;
  /// Number of symbols per subframe.
  unsigned nof_symbols_per_subframe;
  /// \brief Write index for the symbol buffer holding the OFDM symbol being collected.
  ///
  /// Sample index within the \c symbol_buffers data, it points the writing position within the buffered
  /// signal. It is used to copy the samples aligned with the requested timestamp into the destination
  /// buffer.
  unsigned symbol_buffer_write_index;
  /// Index of the symbol buffer the symbol being collected is assembled in.
  unsigned current_symbol_buffer = 0;
  /// Current symbol index within the slot.
  unsigned current_symbol_index;
  /// Current symbol size.
  unsigned current_symbol_size;
  /// Current symbol timestamp.
  baseband_gateway_timestamp current_symbol_timestamp;
  /// Handle of the receive buffer the samples being processed came in (see the interface): every
  /// symbol submitted from this call hands it to the PUxCH, which keeps it until the transform
  /// reading that symbol has been finished. Null when the caller owns the samples for long enough.
  rx_buffer_handle current_owner;
  /// Current slot point.
  slot_point current_slot;
  /// List of the symbol sizes in number samples for each symbol within the subframe.
  std::vector<unsigned> symbol_sizes;
  /// \brief Storage of the baseband samples of the OFDM symbols being collected.
  ///
  /// One buffer per symbol the PUxCH pipeline can keep in flight (see
  /// puxch_processor_baseband::get_nof_symbol_buffers()): a symbol is assembled in the buffer that
  /// acquire_symbol_buffer() hands out, which no transform in flight reads anymore, so the samples of
  /// a symbol are never overwritten while they are still being transformed. The page-aligned variant
  /// is what lets a transform read them without a copy.
  std::vector<baseband_gateway_buffer_dynamic_aligned> symbol_buffers;
  /// Internal PRACH processor.
  std::unique_ptr<prach_processor> prach_proc;
  /// Internal PUxCH processor.
  std::unique_ptr<puxch_processor> puxch_proc;
  /// Uplink processor notifier.
  uplink_processor_notifier* notifier = nullptr;
  /// Carrier Frequency Offset processor.
  baseband_cfo_processor cfo_processor;
  /// Buffer to hold complex floating-point based samples for demodulation.
  dynamic_tensor<2, cf_t> temp_cf_buffer;
};

} // namespace ocudu
