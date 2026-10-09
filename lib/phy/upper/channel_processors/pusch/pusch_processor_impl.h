// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "pusch_processor_notifier_adaptor.h"
#include "pusch_uci_decoder_wrapper.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_decoder.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_demodulator.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_processor.h"
#include "ocudu/phy/upper/channel_processors/pusch/ulsch_demultiplex.h"
#include "ocudu/phy/upper/channel_processors/uci/uci_decoder.h"
#include "pusch_depth3_receiver.h"
#include "ocudu/phy/upper/signal_processors/pusch/dmrs_pusch_estimator.h"
#include "ocudu/phy/upper/unique_rx_buffer.h"
#include "ocudu/ran/pusch/pusch_constants.h"
#include "ocudu/support/memory_pool/bounded_object_pool.h"
#include <memory>

namespace ocudu {

/// Implements a generic software PUSCH processor.
class pusch_processor_impl : public pusch_processor
{
public:
  /// The current maximum supported number of layers.
  static constexpr unsigned max_nof_layers = 4;

  /// Groups the PUSCH processor dependencies that can be reused locally by the same processing thread.
  class concurrent_dependencies
  {
  public:
    /// Creates the dependencies instance.
    /// \param[in] receiver_     The DEPTH-3 unit: channel estimation + equalization + demapping. Which arm this
    ///                         is (classical, or the AI head) was decided by the factory; this class only
    ///                         routes the two seam calls to it.
    /// \param[in] estimator_    The classical estimator, ALSO held here because the strict policy interrogates
    ///                         the MODULE (whether its results cover the hop, and whether a device claimed one
    ///                         it did not compute). Stage S-1 keeps that interrogation on the classical units so
    ///                         that the arm's behaviour is bit-identical; a real AI arm will have to answer
    ///                         those questions itself, and that is a change S-1 deliberately does not make.
    /// \param[in] demodulator_  The classical demodulator, held for the same reason.
    concurrent_dependencies(std::unique_ptr<dmrs_pusch_estimator> estimator_,
                            std::unique_ptr<pusch_demodulator>    demodulator_,
                            std::unique_ptr<ulsch_demultiplex>    demultiplex_,
                            std::unique_ptr<uci_decoder>          uci_dec_,
                            channel_size                          ce_dims) :
      estimator(std::move(estimator_)),
      demodulator(std::move(demodulator_)),
      demultiplex(std::move(demultiplex_)),
      uci_dec(std::move(uci_dec_)),
      harq_ack_decoder(*uci_dec,
                       pusch_constants::get_max_codeword_size(ce_dims.nof_prb, ce_dims.nof_tx_layers).value()),
      csi_part1_decoder(*uci_dec,
                        pusch_constants::get_max_codeword_size(ce_dims.nof_prb, ce_dims.nof_tx_layers).value()),
      csi_part2_decoder(*uci_dec,
                        pusch_constants::get_max_codeword_size(ce_dims.nof_prb, ce_dims.nof_tx_layers).value())

    {
      ocudu_assert(estimator, "Invalid channel estimator.");
      ocudu_assert(demodulator, "Invalid demodulator.");
      ocudu_assert(demultiplex, "Invalid demultiplex.");
      ocudu_assert(uci_dec, "Invalid UCI decoder.");
    }

    /// \brief Attaches the depth-3 unit. Called ONCE, after construction, by the factory that built it.
    ///
    /// It is a setter rather than a constructor argument because the unit REFERENCES the two modules below,
    /// which therefore have to exist first -- and because the dependencies must own them for the strict policy
    /// to interrogate the very modules the unit runs.
    void set_depth3(std::unique_ptr<pusch_depth3_receiver> receiver_) { receiver = std::move(receiver_); }

    /// The depth-3 unit. The two seam calls go through this; see pusch_depth3_receiver.h.
    pusch_depth3_receiver&     get_depth3() { return *receiver; }
    /// The classical estimator, kept OUTSIDE the seam for the strict policy's interrogation (see the
    /// constructor's note).
    dmrs_pusch_estimator&      get_estimator() { return *estimator; }
    /// The classical demodulator, kept for the same reason.
    pusch_demodulator&         get_demodulator() { return *demodulator; }
    ulsch_demultiplex&         get_demultiplex() { return *demultiplex; }
    pusch_uci_decoder_wrapper& get_harq_ack_decoder() { return harq_ack_decoder; }
    pusch_uci_decoder_wrapper& get_csi_part1_decoder() { return csi_part1_decoder; }
    pusch_uci_decoder_wrapper& get_csi_part2_decoder() { return csi_part2_decoder; }

  private:
    /// The depth-3 unit (classical, or the AI head that delegates to the two units below). Attached by
    /// set_depth3() after construction.
    std::unique_ptr<pusch_depth3_receiver> receiver;
    /// Channel estimator instance. The dependencies OWN it; the depth-3 unit holds a reference, so the
    /// strict policy interrogates the very module the unit runs (see the factory).
    std::unique_ptr<dmrs_pusch_estimator> estimator;
    /// Demodulator instance. Owned here and referenced by the depth-3 unit, for the same reason.
    std::unique_ptr<pusch_demodulator> demodulator;
    /// Channel demultiplex.
    std::unique_ptr<ulsch_demultiplex> demultiplex;
    /// UCI Decoder instance.
    std::unique_ptr<uci_decoder> uci_dec;
    /// HARQ-ACK decoder wrapper.
    pusch_uci_decoder_wrapper harq_ack_decoder;
    /// CSI Part 1 decoder wrapper.
    pusch_uci_decoder_wrapper csi_part1_decoder;
    /// CSI Part 2 decoder wrapper.
    pusch_uci_decoder_wrapper csi_part2_decoder;
  };

  /// Dependencies pool class.
  using concurrent_dependencies_pool_type = bounded_unique_object_pool<concurrent_dependencies>;

  /// Collects the necessary parameters for creating a PUSCH processor.
  struct configuration {
    /// Dependencies pool.
    std::shared_ptr<concurrent_dependencies_pool_type> dependencies_pool;
    /// Decoder instance. Ownership is transferred to the processor.
    std::unique_ptr<pusch_decoder> decoder;
    /// Selects the number of LDPC decoder iterations.
    unsigned dec_nof_iterations;
    /// Enables LDPC decoder early stop if the CRC matches before completing \c ldpc_nof_iterations iterations.
    bool dec_enable_early_stop;
    /// PUSCH SINR calculation method for CSI reporting.
    channel_state_information::sinr_type csi_sinr_calc_method;
    /// Channel size as seen by the PUSCH channel estimator.
    channel_size ce_dims;
  };

  /// \brief Constructs a generic software PUSCH processor.
  /// \param[in] config PUSCH processor dependencies and configuration parameters.
  pusch_processor_impl(configuration& config);

  // See interface for documentation.
  void process(span<uint8_t>                    data,
               unique_rx_buffer                 rm_buffer,
               pusch_processor_result_notifier& notifier,
               const resource_grid_reader&      grid,
               const pdu_t&                     pdu) override;

private:
  /// \brief Notifier for the PUSCH channel estimator.
  ///
  /// At the notification by the DM-RS PUSCH estimator, the notified PUSCH processor recovers the PUSCH data by running
  /// the demodulation and decoding steps.
  ///
  /// The notifier is available only after calling the \c configure public method.
  class dmrs_pusch_estimator_notifier_impl : private dmrs_pusch_estimator_notifier
  {
  public:
    /// Constructor: initializes the reference to the notified PUSCH processor.
    explicit dmrs_pusch_estimator_notifier_impl(pusch_processor_impl& pp) : notified_processor(pp) {}

    /// \brief Configures the notifier for the current PUSCH transmission.
    /// \param[out]    data_        Received transport block.
    /// \param[in,out] rm_buffer_   Rate matcher buffer.
    /// \param[in,out] dependencies Pointer to the dependencies object assigned to the PUSCH processor.
    /// \param[in]     notifier_    Result notification interface.
    /// \param[in]     grid_        Source resource grid.
    /// \param[in]     pdu_         Necessary parameters to process the PUSCH transmission.
    /// \param[in]     dmrs_type_   DM-RS type used by the PUSCH transmission.
    /// \param[in]     cdm_         Number of CDM groups without data in the PUSCH transmission.
    /// \return A reference to the configured notifier.
    dmrs_pusch_estimator_notifier& configure(span<uint8_t>                          data_,
                                             unique_rx_buffer                       rm_buffer_,
                                             concurrent_dependencies_pool_type::ptr dependencies_,
                                             pusch_processor_result_notifier&       notifier_,
                                             const resource_grid_reader&            grid_,
                                             const pdu_t&                           pdu_,
                                             dmrs_config_type                       dmrs_type_,
                                             unsigned                               cdm_)
    {
      // Set new PUSCH reception parameters. Use exchange for verifying that previous receptions are not overwritten.
      [[maybe_unused]] auto        prev_data         = std::exchange(data, data_);
      [[maybe_unused]] auto        prev_rm_buffer    = std::exchange(rm_buffer, std::move(rm_buffer_));
      [[maybe_unused]] auto*       prev_notifier     = std::exchange(notifier, &notifier_);
      [[maybe_unused]] auto        prev_dependencies = std::exchange(dependencies, std::move(dependencies_));
      [[maybe_unused]] const auto* prev_grid         = std::exchange(grid, &grid_);
      [[maybe_unused]] const auto* prev_pdu          = std::exchange(pdu, &pdu_);
      used_dmrs_type                                 = dmrs_type_;
      nof_cdm_groups_without_data                    = cdm_;

      // Ensure that no parameter was overwritten.
      ocudu_assert(prev_data.data() == nullptr, "Detected data overwrite.");
      ocudu_assert(!prev_rm_buffer.is_valid(), "Detected RM buffer overwrite.");
      ocudu_assert(prev_notifier == nullptr, "Detected notifier overwrite.");
      ocudu_assert(prev_dependencies == nullptr, "Detected dependencies overwrite.");
      ocudu_assert(prev_grid == nullptr, "Detected grid overwrite.");
      ocudu_assert(prev_pdu == nullptr, "Detected PDU overwrite.");

      // Return its own reference to the estimator callback interface.
      return *this;
    }

    /// \brief The dependencies this reception is using, or nullptr when idle.
    ///
    /// Exposed because the DEPTH-3 seam needs them AFTER \ref configure has taken the caller's handle: the
    /// caller's \c concurrent_dependencies_pool_type::ptr is moved in, so it is null by the time the unit runs.
    /// The classical path did not need this -- it held a reference to the estimator, which survives the move --
    /// but an arm chosen at the granularity of the whole unit has to reach the unit, and the unit lives here.
    concurrent_dependencies* get_dependencies() { return dependencies.get(); }


  private:
    // See interface for documentation.
    void on_estimation_complete(const dmrs_pusch_estimator_results& est_results) override
    {
      // Move current transmission parameters to the stack.
      pusch_processor_result_notifier* current_notifier = std::exchange(notifier, nullptr);
      const resource_grid_reader*      current_grid     = std::exchange(grid, nullptr);
      const pdu_t*                     current_pdu      = std::exchange(pdu, nullptr);
      span<uint8_t>                    current_data     = std::exchange(data, {});

      // Verify the parameters are valid before continuing with the processing.
      ocudu_assert(current_notifier != nullptr, "Invalid notifier.");
      ocudu_assert(current_grid != nullptr, "Invalid grid.");
      ocudu_assert(current_pdu != nullptr, "Invalid PDU.");
      ocudu_assert((current_data.data() != nullptr) && (!current_data.empty()), "Invalid data.");

      // Keep processing the PUSCH reception. The notifier can be configured for a different reception before returning.
      notified_processor.process_data(current_data,
                                      std::move(rm_buffer),
                                      std::move(dependencies),
                                      *current_notifier,
                                      est_results,
                                      *current_grid,
                                      *current_pdu,
                                      used_dmrs_type,
                                      nof_cdm_groups_without_data);
    }

    /// Reference to the PUSCH processor waiting for notification.
    pusch_processor_impl& notified_processor;
    /// Pointer to the dependencies object assigned to the PUSCH processor.
    concurrent_dependencies_pool_type::ptr dependencies;
    /// Buffer to store retrieved data.
    span<uint8_t> data;
    /// Rate matcher buffer.
    unique_rx_buffer rm_buffer;
    /// Pointer to the PUSCH processor notifier passed to the notified processor.
    pusch_processor_result_notifier* notifier = nullptr;
    /// Pointer to the reader of the resource grid containing the PUSCH transmission.
    const resource_grid_reader* grid = nullptr;
    /// Pointer to the PDU describing the processed PUSCH transmission.
    const pdu_t* pdu = nullptr;
    /// DM-RS type.
    dmrs_config_type used_dmrs_type = dmrs_config_type::type1;
    /// Number of CDM groups without data.
    unsigned nof_cdm_groups_without_data = 2;
  };
  /// Channel estimator notifier configurator.
  dmrs_pusch_estimator_notifier_impl estimator_notifier_configurator;

  ocudulog::basic_logger& logger;
  /// Dependencies pool.
  std::shared_ptr<concurrent_dependencies_pool_type> dependencies_pool;
  /// UL-SCH transport block decoder.
  std::unique_ptr<pusch_decoder> decoder;
  /// Selects the number of LDPC decoder iterations.
  unsigned dec_nof_iterations;
  /// Enables LDPC decoder early stop if the CRC matches before completing \c ldpc_nof_iterations iterations.
  bool dec_enable_early_stop;
  /// \brief Channel size as seen by the PUSCH channel estimator.
  ///
  /// Coincides with the maximum PDU size.
  channel_size ce_dims;
  /// Selects the PUSCH SINR calculation method.
  channel_state_information::sinr_type csi_sinr_calc_method;
  /// Notifier adaptor.
  pusch_processor_notifier_adaptor notifier_adaptor;

  /// \brief Processes the data in a PUSCH transmission.
  /// \param[out]    data                         Received transport block.
  /// \param[in,out] rm_buffer                    Rate matcher buffer.
  /// \param[in,out] dependencies                 Pointer to the dependencies object assigned to the PUSCH processor.
  /// \param[in]     notifier                     Result notification interface.
  /// \param[in]     est_results                  Results of the DM-RS channel estimator.
  /// \param[in]     grid                         Source resource grid.
  /// \param[in]     pdu                          Necessary parameters to process the PUSCH transmission.
  /// \param[in]     dmrs_type                    DM-RS type used by the PUSCH transmission.
  /// \param[in]     nof_cdm_groups_without_data  Number of CDM groups without data in the PUSCH transmission.
  void process_data(span<uint8_t>                          data,
                    unique_rx_buffer                       rm_buffer,
                    concurrent_dependencies_pool_type::ptr dependencies,
                    pusch_processor_result_notifier&       notifier,
                    const dmrs_pusch_estimator_results&    est_results,
                    const resource_grid_reader&            grid,
                    const pdu_t&                           pdu,
                    dmrs_config_type                       dmrs_type,
                    unsigned                               nof_cdm_groups_without_data);
};

} // namespace ocudu
