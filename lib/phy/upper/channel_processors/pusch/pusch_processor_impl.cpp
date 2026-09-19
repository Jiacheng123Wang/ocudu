// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "pusch_processor_impl.h"

#include "ul_capture.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <optional>
#include <vector>
#include <cstdlib>
#include <string>
#include "pusch_decoder_buffer_dummy.h"
#include "pusch_processor_notifier_adaptor.h"
#include "pusch_processor_validator_impl.h"
#include "ocudu/phy/support/resource_grid_reader.h"
#include "ocudu/adt/format.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/phy/phy_pipeline_crossings.h"
#include "ocudu/phy/phy_pipeline_strict.h"
#include "ocudu/phy/upper/channel_coding/ldpc/ldpc.h"
#include "ocudu/phy/upper/channel_processors/pusch/formatters.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_codeword_buffer.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_decoder_buffer.h"
#include "ocudu/phy/upper/unique_rx_buffer.h"
#include "ocudu/ran/pusch/ulsch_info.h"
#include "ocudu/ran/sch/sch_dmrs_power.h"
#include "ocudu/ran/uci/uci_formatters.h"
#include "ocudu/ran/uci/uci_part2_size_calculator.h"
#include "ocudu/support/executors/ul_pipeline_probe.h"

using namespace ocudu;

#if defined(OCUDU_METAL_STATS)
namespace {

/// \brief Grants failed by the fused lane's strict policy, and the reason that caused the first one.
///
/// The per-hop ERROR is the event; this is the number, reported at exit like every other measurement
/// of this line (see the contract in phy_pipeline_contract.h).
std::atomic<uint64_t>& strict_failure_count()
{
  static std::atomic<uint64_t>* v = new std::atomic<uint64_t>(0);
  return *v;
}

const char*& strict_first_reason()
{
  static const char* reason = nullptr;
  return reason;
}

const bool strict_report_registered = []() {
  std::atexit([]() {
    const uint64_t n = strict_failure_count().load(std::memory_order_relaxed);
    if (n == 0) {
      return;
    }
    std::fprintf(stderr,
                 "[phy_pipeline] strict: %llu grant(s) failed because the device did not cover the hop "
                 "(mode=gpu; first reason: %s)\n",
                 static_cast<unsigned long long>(n),
                 (strict_first_reason() != nullptr) ? strict_first_reason() : "unreported");
  });
  return true;
}();

} // namespace
#endif

/// \brief Looks at the output of the validator and, if unsuccessful, fills \c msg with the error message.
///
/// This is used to call the validator inside the process methods only if asserts are active.
[[maybe_unused]] static bool handle_validation(std::string& msg, const error_type<std::string>& err)
{
  bool is_success = err.has_value();
  if (!is_success) {
    msg = err.error();
  }
  return is_success;
}

namespace {
class pusch_processor_csi_part1_feedback_impl : public pusch_processor_csi_part1_feedback
{
public:
  pusch_processor_csi_part1_feedback_impl(pusch_uci_decoder_wrapper& csi_part2_decoder_,
                                          pusch_decoder&             ulsch_decoder_,
                                          ulsch_demultiplex&         demultiplex_,
                                          modulation_scheme          modulation_,
                                          uci_part2_size_description csi_part2_size_,
                                          ulsch_configuration        ulsch_config_) :
    csi_part2_decoder(csi_part2_decoder_),
    ulsch_decoder(ulsch_decoder_),
    demultiplex(demultiplex_),
    modulation(modulation_),
    csi_part2_size(std::move(csi_part2_size_)),
    ulsch_config(std::move(ulsch_config_))
  {
  }

  void connect_notifier(pusch_processor_notifier_adaptor& notifier_) { notifier = &notifier_; }

  void on_csi_part1(const uci_payload_type& part1) override
  {
    ocudu_assert(notifier != nullptr, "Notifier not connected.");

    unsigned nof_csi_part_2_bits = uci_part2_get_size(part1, csi_part2_size).value();

    // Skip if the number of CSI Part 2 bits is zero.
    if (nof_csi_part_2_bits == 0) {
      return;
    }

    // Update the number of CSI Part 2 bits.
    ulsch_config.nof_csi_part2_bits = units::bits(nof_csi_part_2_bits);

    // Recalculate the UL-SCH information.
    ulsch_information info = get_ulsch_information(ulsch_config);

    // Get CSI Part 2 notifier.
    pusch_uci_decoder_notifier& csi_part2_notifier = notifier->get_csi_part2_notifier();

    // Configure CSI Part 2 decoder.
    pusch_decoder_buffer& csi_part2_buffer =
        csi_part2_decoder.new_transmission(nof_csi_part_2_bits, modulation, csi_part2_notifier);

    // Configure UL-SCH demultiplex.
    demultiplex.set_csi_part2(csi_part2_buffer, nof_csi_part_2_bits, info.nof_csi_part2_bits.value());

    // Set the number of UL-SCH softbits in the PUSCH decoder.
    ulsch_decoder.set_nof_softbits(info.nof_ul_sch_bits);
  }

private:
  pusch_processor_notifier_adaptor* notifier;
  pusch_uci_decoder_wrapper&        csi_part2_decoder;
  pusch_decoder&                    ulsch_decoder;
  ulsch_demultiplex&                demultiplex;
  modulation_scheme                 modulation;
  uci_part2_size_description        csi_part2_size;
  ulsch_configuration               ulsch_config;
};

} // namespace

// Dummy PUSCH decoder buffer. Used for PUSCH transmissions without SCH data.
static pusch_decoder_buffer_dummy decoder_buffer_dummy;

pusch_processor_impl::pusch_processor_impl(configuration& config) :
  estimator_notifier_configurator(*this),
  logger(ocudulog::fetch_basic_logger("PHY")),
  dependencies_pool(std::move(config.dependencies_pool)),
  decoder(std::move(config.decoder)),
  dec_nof_iterations(config.dec_nof_iterations),
  dec_enable_early_stop(config.dec_enable_early_stop),
  ce_dims(config.ce_dims),
  csi_sinr_calc_method(config.csi_sinr_calc_method)
{
  ocudu_assert(dependencies_pool, "Invalid dependency pool.");
  ocudu_assert(decoder, "Invalid decoder.");
  ocudu_assert(dec_nof_iterations != 0, "The decoder number of iterations must be non-zero.");
}


namespace {

} // namespace

void pusch_processor_impl::process(span<uint8_t>                    data,
                                   unique_rx_buffer                 rm_buffer,
                                   pusch_processor_result_notifier& notifier,
                                   const resource_grid_reader&      grid,
                                   const pusch_processor::pdu_t&    pdu)
{
  // Get dependencies.
  concurrent_dependencies_pool_type::ptr dependencies = dependencies_pool->get();

  if (!dependencies) {
    logger.error("Failed to retrieve PUSCH processor dependencies.");

    // Notify
    if (pdu.uci.nof_harq_ack != 0) {
      notifier.on_uci({.harq_ack  = {.payload = uci_payload_type(pdu.uci.nof_harq_ack), .status = uci_status::invalid},
                       .csi_part1 = {},
                       .csi_part2 = {},
                       .csi       = {}});
    }

    // Notify the completion of the data processing as the CRC check is KO.
    if (pdu.codeword.has_value()) {
      notifier.on_sch({});
    }

    return;
  }

  // Assert PDU.
  [[maybe_unused]] std::string msg;
  ocudu_assert(handle_validation(msg, pusch_processor_validator_impl(ce_dims).is_valid(pdu)), "{}", msg);

  // Debug capture of the received grid and the PDU (see ul_capture): no-op unless OCUDU_UL_DUMP
  // is set.
  ul_capture::set_current(pdu.slot, pdu.rnti);
  {
    // A debug capture is NOT the CPU participating in the lane (design document, the 2026-09-20
    // ruling): whatever host touches it forces are counted as DEBUG and the crossing contract does not
    // judge them - and a release build does not compile the machinery at all (ENABLE_UL_CAPTURE).
    phy_pipeline_crossings::scoped_debug_touches debug_capture;
    ul_capture::capture_grid(grid, pdu);
  }

  // Get RB mask relative to Point A. It assumes PUSCH is never interleaved.
  crb_bitmap rb_mask = pdu.freq_alloc.get_crb_mask(pdu.bwp_start_rb, pdu.bwp_size_rb);

  bool             enable_transform_precoding  = false;
  unsigned         scrambling_id               = 0;
  unsigned         n_rs_id                     = 0;
  bool             n_scid                      = false;
  unsigned         nof_cdm_groups_without_data = 2;
  dmrs_config_type dmrs_type                   = dmrs_config_type::type1;
  if (std::holds_alternative<ocudu::pusch_processor::dmrs_configuration>(pdu.dmrs)) {
    const auto& dmrs_config     = std::get<ocudu::pusch_processor::dmrs_configuration>(pdu.dmrs);
    scrambling_id               = dmrs_config.scrambling_id;
    n_scid                      = dmrs_config.n_scid;
    nof_cdm_groups_without_data = dmrs_config.nof_cdm_groups_without_data;
    dmrs_type                   = dmrs_config.dmrs;
  } else {
    const auto& dmrs_config    = std::get<ocudu::pusch_processor::dmrs_transform_precoding_configuration>(pdu.dmrs);
    enable_transform_precoding = true;
    n_rs_id                    = dmrs_config.n_rs_id;
  }

  // Configure the channel estimator.
  dmrs_pusch_estimator::configuration ch_est_config;
  ch_est_config.slot = pdu.slot;
  if (enable_transform_precoding) {
    ch_est_config.sequence_config = dmrs_pusch_estimator::low_papr_sequence_configuration{.n_rs_id = n_rs_id};
  } else {
    ch_est_config.sequence_config = dmrs_pusch_estimator::pseudo_random_sequence_configuration{
        .type = dmrs_type, .nof_tx_layers = pdu.nof_tx_layers, .scrambling_id = scrambling_id, .n_scid = n_scid};
  }
  ch_est_config.scaling      = convert_dB_to_amplitude(-get_sch_to_dmrs_ratio_dB(nof_cdm_groups_without_data));
  ch_est_config.c_prefix     = pdu.cp;
  ch_est_config.symbols_mask = pdu.dmrs_symbol_mask;
  ch_est_config.rb_mask      = rb_mask;
  ch_est_config.first_symbol = pdu.start_symbol_index;
  ch_est_config.nof_symbols  = pdu.nof_symbols;
  ch_est_config.rx_ports.assign(pdu.rx_ports.begin(), pdu.rx_ports.end());
  // The DC subcarrier carries no data: tell the estimator so that an estimator building the
  // equalizer's input on the device erases that resource element itself (the host path erases it
  // when it gathers the estimates).
  ch_est_config.dc_position = pdu.dc_position;

  // Configure and get the estimator notifier.
  dmrs_pusch_estimator&          estimator          = dependencies->get_estimator();
  dmrs_pusch_estimator_notifier& estimator_notifier = estimator_notifier_configurator.configure(
      data, std::move(rm_buffer), std::move(dependencies), notifier, grid, pdu, dmrs_type, nof_cdm_groups_without_data);

  // Run the channel estimator. When done, the notifier will trigger the remaining steps for recovering the PUSCH data.
  estimator.estimate(estimator_notifier, grid, ch_est_config);
}

void pusch_processor_impl::process_data(span<uint8_t>                          data,
                                        unique_rx_buffer                       rm_buffer,
                                        concurrent_dependencies_pool_type::ptr dependencies,
                                        pusch_processor_result_notifier&       notifier,
                                        const dmrs_pusch_estimator_results&    est_results,
                                        const resource_grid_reader&            grid,
                                        const pdu_t&                           pdu,
                                        dmrs_config_type                       dmrs_type,
                                        unsigned                               nof_cdm_groups_without_data)
{
  // Debug capture of the channel estimator results (no-op unless OCUDU_UL_DUMP is set). It is the
  // stage that decides the equalizer's noise variance and hence the soft-bit scale. The key is set
  // again here because this function may run on a different thread than process().
  ul_capture::set_current(pdu.slot, pdu.rnti);

  // ---- The fused lane's strict policy (user ruling, see phy_pipeline_strict.h) -------------------
  // The host would compute this hop from here on - gather the estimates, equalize, demodulate - and the
  // CPU would be back in the loop, which is the one thing mode=gpu claims never happens. The ruling is
  // that this is an ERROR, so the grant fails HERE, before a single soft bit exists: the same shape the
  // missing-dependencies path in process() uses (an ERROR, an empty SCH result = CRC KO so the MAC
  // retransmits, and a return). A hop refused by a KNOB is exempt: the A/B arms exist to take the host
  // route.
  //
  // There are TWO ways the host can end up doing the work, and both have to be closed:
  //   * the estimator did not cover the hop (its refusal reason is what gets reported), or
  //   * the estimator covered it, but the demodulator cannot read it where it was produced - a topology
  //     with more than one receive port or layer (see channel_equalizer::consumes_device_estimates).
  // That second question is asked of the demodulator itself, so the answer cannot drift from what
  // demodulate() is about to do.
  if (phy_pipeline_strict_enabled() && !est_results.device_shortfall_is_knob_requested()) {
    const bool estimator_covered = est_results.device_results_cover_last_estimate();
    const bool topology_in_place =
        dependencies->get_demodulator().serves_hop_in_place(est_results, pdu.rx_ports.size(), pdu.nof_tx_layers);
    if (!estimator_covered || !topology_in_place) {
      const char* reported = est_results.device_shortfall_reason();
      const char* cause =
          estimator_covered
              ? "the device route does not read this topology in place (more than one port or layer)"
              : ((reported != nullptr) ? reported : "no stage reported a cause");
      // Two sinks on purpose, and they are two DIFFERENT files: ocudulog writes the gNB's log (stdout),
      // while everything this line's measurements are read from goes to stderr - so the operator sees the
      // error where errors belong, and a leg's analysis (and the criterion for this policy) can grep the
      // event without parsing the logger's format.
      logger.error("PUSCH: slot={} rnti={} the device did not cover this hop ({}), and mode=gpu does not let "
                   "the host compute it - failing the grant",
                   pdu.slot,
                   pdu.rnti,
                   cause);
      const std::string marker = fmt::format(
          "[phy_pipeline] strict: slot={} rnti={} the device did not cover the hop ({}) - the PUSCH fails "
          "instead of being computed on the host\n",
          pdu.slot,
          pdu.rnti,
          cause);
      std::fprintf(stderr, "%s", marker.c_str());
#if defined(OCUDU_METAL_STATS)
      // One number at exit: it turns "the log flooded" into "it happened N times, first because of X".
      strict_failure_count().fetch_add(1, std::memory_order_relaxed);
      if (strict_first_reason() == nullptr) {
        strict_first_reason() = cause;
      }
#endif
      if (pdu.uci.nof_harq_ack != 0) {
        notifier.on_uci(
            {.harq_ack = {.payload = uci_payload_type(pdu.uci.nof_harq_ack), .status = uci_status::invalid},
             .csi_part1 = {},
             .csi_part2 = {},
             .csi       = {}});
      }
      if (pdu.codeword.has_value()) {
        notifier.on_sch({});
      }
      return;
    }
  }

  if (ul_capture::enabled()) {
    // The capture reads host copies of the estimator's results, so it needs them complete. With a
    // deferred estimator that costs the overlap the demodulation below would get, which is the
    // right trade for a debug capture - and it is why this is gated instead of unconditional.
    phy_pipeline_crossings::scoped_debug_touches debug_capture;
    est_results.sync_device_estimates();
    ul_capture::capture_ce(est_results, pdu);
    ul_capture::capture_h(est_results, pdu);
  }

  using namespace units::literals;

  // The channel estimator has finished: the channel estimates of all the data symbols of the slot are ready.
  // End timestamp of the channel estimation phase segment (and start of the equalization+demodulation one).
  ul_pipeline_probe::get().record_ce_end(pdu.slot.count());

  // Get RB mask relative to Point A. According to TS38.211 Section 6.3.1.7, the VRB-to-PRB mapping for PUSCH is never
  // interleaved.
  crb_bitmap rb_mask = pdu.freq_alloc.get_crb_mask(pdu.bwp_start_rb, pdu.bwp_size_rb);

  // Note: the channel estimator's measurements (RSRP, EPRE, noise, time alignment, CFO, its own
  // SINR) are merged into the reported Channel State Information at the end of this function, once
  // its results are complete - the demodulation in between is what overlaps the estimator's device
  // work when the estimator defers it. They are fields the demodulator does not write, so merging
  // them there preserves its post-equalization SINR and EVM.

  // Number of RB used by this transmission.
  unsigned nof_rb = pdu.freq_alloc.get_nof_rb();

  // Determine if the PUSCH allocation overlaps with the position of the DC.
  bool overlap_dc = false;
  if (pdu.dc_position.has_value()) {
    unsigned dc_position_prb = *pdu.dc_position / NOF_SUBCARRIERS_PER_RB;
    overlap_dc               = rb_mask.test(dc_position_prb);
  }

  // Configure the UL SCH transmission.
  ulsch_configuration ulsch_config;
  ulsch_config.tbs                         = units::bytes(data.size()).to_bits();
  ulsch_config.mcs_descr                   = pdu.mcs_descr;
  ulsch_config.nof_harq_ack_bits           = units::bits(pdu.uci.nof_harq_ack);
  ulsch_config.nof_csi_part1_bits          = units::bits(pdu.uci.nof_csi_part1);
  ulsch_config.nof_csi_part2_bits          = 0_bits;
  ulsch_config.alpha_scaling               = pdu.uci.alpha_scaling;
  ulsch_config.beta_offset_harq_ack        = pdu.uci.beta_offset_harq_ack;
  ulsch_config.beta_offset_csi_part1       = pdu.uci.beta_offset_csi_part1;
  ulsch_config.beta_offset_csi_part2       = pdu.uci.beta_offset_csi_part2;
  ulsch_config.nof_rb                      = nof_rb;
  ulsch_config.start_symbol_index          = pdu.start_symbol_index;
  ulsch_config.nof_symbols                 = pdu.nof_symbols;
  ulsch_config.dmrs_type                   = dmrs_type;
  ulsch_config.dmrs_symbol_mask            = pdu.dmrs_symbol_mask;
  ulsch_config.nof_cdm_groups_without_data = nof_cdm_groups_without_data;
  ulsch_config.nof_layers                  = pdu.nof_tx_layers;
  ulsch_config.contains_dc                 = overlap_dc;

  // Prepare demultiplex configuration.
  ulsch_information                info = get_ulsch_information(ulsch_config);
  ulsch_demultiplex::configuration demux_config;
  demux_config.modulation                  = pdu.mcs_descr.modulation;
  demux_config.nof_layers                  = pdu.nof_tx_layers;
  demux_config.nof_prb                     = ulsch_config.nof_rb;
  demux_config.start_symbol_index          = pdu.start_symbol_index;
  demux_config.nof_symbols                 = pdu.nof_symbols;
  demux_config.nof_harq_ack_rvd            = info.nof_harq_ack_rvd.value();
  demux_config.dmrs                        = dmrs_type;
  demux_config.dmrs_symbol_mask            = ulsch_config.dmrs_symbol_mask;
  demux_config.nof_cdm_groups_without_data = ulsch_config.nof_cdm_groups_without_data;
  demux_config.nof_harq_ack_bits           = ulsch_config.nof_harq_ack_bits.value();
  demux_config.nof_enc_harq_ack_bits       = info.nof_harq_ack_bits.value();
  demux_config.nof_csi_part1_bits          = ulsch_config.nof_csi_part1_bits.value();
  demux_config.nof_enc_csi_part1_bits      = info.nof_csi_part1_bits.value();

  bool has_sch_data = pdu.codeword.has_value();

  // Prepare decoder buffers with dummy instances.
  std::reference_wrapper<pusch_decoder_buffer> decoder_buffer(decoder_buffer_dummy);
  std::reference_wrapper<pusch_decoder_buffer> harq_ack_buffer(decoder_buffer_dummy);
  std::reference_wrapper<pusch_decoder_buffer> csi_part1_buffer(decoder_buffer_dummy);

  // Prepare CSI Part 1 feedback.
  pusch_processor_csi_part1_feedback_impl csi_part1_feedback(dependencies->get_csi_part2_decoder(),
                                                             *decoder,
                                                             dependencies->get_demultiplex(),
                                                             pdu.mcs_descr.modulation,
                                                             pdu.uci.csi_part2_size,
                                                             ulsch_config);

  // Prepare notifiers.
  notifier_adaptor.new_transmission(notifier, csi_part1_feedback, csi_sinr_calc_method, has_sch_data ? data.size() : 0);
  notifier_adaptor.set_slot(pdu.slot);
  csi_part1_feedback.connect_notifier(notifier_adaptor);

  if (has_sch_data) {
    units::bits tbs            = units::bytes(data.size()).to_bits();
    unsigned    nof_codeblocks = compute_nof_codeblocks(tbs, pdu.codeword->ldpc_base_graph);
    units::bits Nref           = ldpc::compute_N_ref(pdu.tbs_lbrm, nof_codeblocks);

    // Prepare decoder configuration.
    pusch_decoder::configuration decoder_config;
    decoder_config.base_graph          = pdu.codeword->ldpc_base_graph;
    decoder_config.rv                  = pdu.codeword->rv;
    decoder_config.mod                 = pdu.mcs_descr.modulation;
    decoder_config.Nref                = Nref.value();
    decoder_config.nof_layers          = pdu.nof_tx_layers;
    decoder_config.nof_ldpc_iterations = dec_nof_iterations;
    decoder_config.use_early_stop      = dec_enable_early_stop;
    decoder_config.new_data            = pdu.codeword->new_data;
    decoder_config.slot                = pdu.slot;

    // Setup decoder.
    decoder_buffer =
        decoder->new_data(data, std::move(rm_buffer), notifier_adaptor.get_sch_data_notifier(), decoder_config);

    // If there is no expected CSI Part 2 payload, the number of UL-SCH LLRs is known without the need to decode the
    // CSI Part 1 payload.
    if (pdu.uci.csi_part2_size.entries.empty()) {
      decoder->set_nof_softbits(info.nof_ul_sch_bits);
    }
  }

  // Prepares HARQ-ACK notifier and buffer.
  if (pdu.uci.nof_harq_ack != 0) {
    harq_ack_buffer = dependencies->get_harq_ack_decoder().new_transmission(
        pdu.uci.nof_harq_ack, pdu.mcs_descr.modulation, notifier_adaptor.get_harq_ack_notifier());
  }

  // Prepares CSI Part 1 notifier and buffer.
  if (pdu.uci.nof_csi_part1 != 0) {
    csi_part1_buffer = dependencies->get_csi_part1_decoder().new_transmission(
        pdu.uci.nof_csi_part1, pdu.mcs_descr.modulation, notifier_adaptor.get_csi_part1_notifier());
  }

  // Demultiplex SCH data, HARQ-ACK and CSI Part 1.
  pusch_codeword_buffer& demodulator_buffer =
      dependencies->get_demultiplex().demultiplex(decoder_buffer, harq_ack_buffer, csi_part1_buffer, demux_config);

  // Demodulate.
  bool enable_transform_precoding = !std::holds_alternative<ocudu::pusch_processor::dmrs_configuration>(pdu.dmrs);

  pusch_demodulator::configuration demod_config;
  demod_config.rnti                        = pdu.rnti;
  demod_config.rb_mask                     = pdu.freq_alloc.get_crb_mask(pdu.bwp_start_rb, pdu.bwp_size_rb);
  demod_config.modulation                  = pdu.mcs_descr.modulation;
  demod_config.start_symbol_index          = pdu.start_symbol_index;
  demod_config.nof_symbols                 = pdu.nof_symbols;
  demod_config.dmrs_symb_pos               = pdu.dmrs_symbol_mask;
  demod_config.dmrs_type                   = demux_config.dmrs;
  demod_config.nof_cdm_groups_without_data = ulsch_config.nof_cdm_groups_without_data;
  demod_config.n_id                        = pdu.n_id;
  demod_config.nof_tx_layers               = pdu.nof_tx_layers;
  demod_config.dc_position                 = pdu.dc_position;
  demod_config.enable_transform_precoding  = enable_transform_precoding;
  demod_config.rx_ports                    = pdu.rx_ports;
  demod_config.n_rapid                     = pdu.n_rapid;
  // G-5 DD-label data hook: dump the received (pre-equalization) resource grid REs
  // of this PUSCH allocation, ordered [port][symbol][re] as float32 (re,im) pairs.
  if (const char* dump_dir = std::getenv("OCUDU_HELENA_DUMP_DIR"); dump_dir != nullptr) {
    static std::atomic<unsigned> rx_idx{0};
    const unsigned               idx  = rx_idx.fetch_add(1, std::memory_order_relaxed);
    const auto                   t_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                           std::chrono::steady_clock::now().time_since_epoch())
                                           .count();
    const unsigned               n_prb = rb_mask.count();
    const unsigned               k0    = rb_mask.find_lowest() * NOF_SUBCARRIERS_PER_RB;
    char                         path[512];
    std::snprintf(path, sizeof(path), "%s/rx_%08u_prb%u.f32", dump_dir, idx, n_prb);
    FILE* f = std::fopen(path, "wb");
    if (f != nullptr) {
      std::vector<cf_t> sym(n_prb * NOF_SUBCARRIERS_PER_RB);
      for (unsigned l = pdu.start_symbol_index; l != pdu.start_symbol_index + pdu.nof_symbols; ++l) {
        for (unsigned port : pdu.rx_ports) {
          grid.get(sym, port, l, k0, 1);
          std::fwrite(sym.data(), sizeof(cf_t), sym.size(), f);
        }
      }
      std::fclose(f);
    }
    std::snprintf(path, sizeof(path), "%s/rx_meta.csv", dump_dir);
    f = std::fopen(path, "a");
    if (f != nullptr) {
      // Columns: idx,t_us,n_prb,n_syms,n_ports,k0,mod,dmrs_sym_mask,n_id,n_scid,
      // scrambling_id,rnti,n_layers,rv,new_data,slot - everything the offline
      // re-encoder needs to rebuild X_hat from the decoded TB (the DD-label
      // sidecar). The slot column pairs exactly with dd_meta.csv's slot column
      // (single-UE captures): the grant whose decode produced a TB is the rx row
      // with the same slot, immune to the async-decode timestamp jitter.
      unsigned dmrs_mask = 0;
      for (unsigned s = 0; s != MAX_NSYMB_PER_SLOT; ++s) {
        dmrs_mask |= (pdu.dmrs_symbol_mask.test(s) ? 1U : 0U) << s;
      }
      unsigned scr_id   = 0;
      unsigned n_scid_v = 0;
      if (std::holds_alternative<ocudu::pusch_processor::dmrs_configuration>(pdu.dmrs)) {
        const auto& dmrs_cfg = std::get<ocudu::pusch_processor::dmrs_configuration>(pdu.dmrs);
        scr_id               = dmrs_cfg.scrambling_id;
        n_scid_v             = dmrs_cfg.n_scid;
      }
      const unsigned rv       = pdu.codeword.has_value() ? pdu.codeword->rv : 0;
      const unsigned new_data = pdu.codeword.has_value() ? (pdu.codeword->new_data ? 1U : 0U) : 1U;
      std::fprintf(f, "%u,%lld,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n", idx,
                   static_cast<long long>(t_us), n_prb, pdu.nof_symbols,
                   static_cast<unsigned>(pdu.rx_ports.size()), k0,
                   static_cast<unsigned>(pdu.mcs_descr.modulation), dmrs_mask, pdu.n_id,
                   n_scid_v, scr_id,
                   static_cast<unsigned>(pdu.rnti), pdu.nof_tx_layers, rv, new_data,
                   pdu.slot.system_slot());
      std::fclose(f);
    }
  }

  dependencies->get_demodulator().demodulate(
      demodulator_buffer, notifier_adaptor.get_demodulator_notifier(), grid, est_results, demod_config);

  // The demodulation is done: complete the channel estimation and merge its measurements into the
  // reported Channel State Information. Reading them any earlier would put the whole demodulation
  // behind the estimator's synchronization.
  (void)est_results.sync_device_estimates();
  est_results.get_channel_state_information(notifier_adaptor.get_channel_state_information());
}
