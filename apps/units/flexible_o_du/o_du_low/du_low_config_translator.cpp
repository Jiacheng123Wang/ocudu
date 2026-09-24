// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "du_low_config_translator.h"
#include "apps/services/worker_manager/worker_manager_config.h"
#include "du_low_config.h"
#include "du_low_phy_pipeline.h"
#include "ocudu/adt/format.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/phy/upper/channel_coding/ldpc/ldpc.h"
#include "ocudu/phy/upper/upper_phy_factories.h"
#include "ocudu/ran/duplex_mode.h"
#include "ocudu/ran/prach/prach_configuration.h"
#include "ocudu/ran/pusch/pusch_constants.h"
#include "ocudu/support/cpu_architecture_info.h"
#include <cmath>
#include <cstdio>
#include <string>

using namespace ocudu;

/// Describes one effective module backend for the startup log, flagging the substitutions applied because the
/// requested backend is not built into this binary.
static std::string describe_backend(const std::string& requested, const std::string& effective)
{
  if (requested == effective) {
    return effective;
  }
  if (requested == "auto") {
    // "auto" is the "follow the pipeline mode" value, not a fallback: say which backend it resolved to.
    return fmt::format("{} (auto)", effective);
  }
  return fmt::format("{}->{} (requested backend not built in)", requested, effective);
}

/// \brief Logs the effective uplink PHY pipeline configuration, once at startup.
///
/// The per-module backend lines are the single place where the user can see what the pipeline mode actually selected;
/// the substitutions applied for backends that are not built into the binary are flagged there as well.
static void log_phy_pipeline_config(const du_low_unit_expert_upper_phy_config& config,
                                    const phy_pipeline_effective&              effective)
{
  // Publish the effective mode for the instrumentation probes (see phy_pipeline_mode_registry).
  phy_pipeline_mode_registry::set(effective.mode);

  ocudulog::basic_logger& logger = ocudulog::fetch_basic_logger("PHY");
  logger.info("[phy_pipeline] mode={} fused={} device_grid={} lane=IQ->LLR (expert_phy --phy_pipeline {})",
              to_string(effective.mode),
              effective.lane_fused ? "yes" : "no",
              effective.device_grid ? "yes" : "no",
              config.phy_pipeline);

  // The demapper has no backend knob of its own: it follows the channel equalizer (see the upper PHY factory).
  const char* demapper =
      is_cpu_phy_backend(effective.equalizer) ? "cpu" : (effective.equalizer == "metal" ? "metal" : "follows-equalizer");
  logger.info("[phy_pipeline]   dft={} channel_estimator={} equalizer={} demapper={} ldpc_decoder={}",
              describe_backend(config.pusch_dft_type, effective.dft),
              describe_backend(config.pusch_channel_estimator_algo, effective.ch_est),
              describe_backend(config.pusch_channel_equalizer_backend, effective.equalizer),
              demapper,
              describe_backend(config.ldpc_decoder_type, effective.ldpc));

  // The mode's promise - no host pass over the samples - is verified by the probes' contract report at
  // exit (see phy_pipeline_contract.h) and enforced before the run starts: the lower PHY factory refuses
  // the gpu mode with a receive buffer that cannot hold a whole slot, because only then is no OFDM
  // symbol split between two receive blocks (see lower_phy_baseband_processor::ul_process). The startup
  // warning that used to stand here is gone with the copy it warned about.
  //
  // The mode is NOT refused any more either (corrected 2026-09-23, milestone audit): this comment used to
  // end with "the mode itself is still refused by the validator ... (the fused lane is not implemented
  // yet)", which stopped being true when that placeholder refusal was deleted from
  // du_low_config_validator.cpp - the air legs have run mode=gpu with `contract MET (8 of 8)` since.
  if ((effective.mode == phy_pipeline_mode::cpu_gpu) && is_cpu_phy_backend(effective.dft) &&
      is_cpu_phy_backend(effective.ch_est) && is_cpu_phy_backend(effective.equalizer) &&
      is_cpu_phy_backend(effective.ldpc)) {
    // Not an error: cpu_gpu with every module on the CPU is exactly the CPU pipeline (the mode is derived from the
    // module knobs, so this is what a command line without any offload knob selects).
    logger.info("[phy_pipeline]   no offload module selected: the effective backend of every module is CPU");
  }
}

static odu::du_low_config generate_du_low_config(const du_low_unit_config&                       du_low,
                                                 span<const o_du_low_unit_config::du_low_config> cells)
{
  odu::du_low_config out_config;
  out_config.cells.reserve(cells.size());

  // Resolve the uplink pipeline mode against the module backend knobs. The result is the single source of truth for
  // every backend handed to the PHY factories below, and for the lower PHY (the DFT reaches the radio unit through
  // the RU configuration, resolved through the same entry point).
  const phy_pipeline_effective effective = resolve_phy_pipeline_or_fatal(du_low.expert_phy_cfg);
  log_phy_pipeline_config(du_low.expert_phy_cfg, effective);

  unsigned max_ul_bw_rb         = 0;
  unsigned pusch_max_nof_layers = 0;
  unsigned max_nof_rx_antennas  = 0;
  for (unsigned i = 0, e = cells.size(); i != e; ++i) {
    const o_du_low_unit_config::du_low_config& cell = cells[i];
    max_ul_bw_rb                                    = std::max(cell.bw_rb, max_ul_bw_rb);
    pusch_max_nof_layers                            = std::max(cell.pusch_max_nof_layers, pusch_max_nof_layers);
    max_nof_rx_antennas                             = std::max(cell.nof_rx_antennas, max_nof_rx_antennas);
  }

  upper_phy_factory_configuration& upper_phy_factory_config = out_config.upper_phy_common_config;
  upper_phy_factory_config.log_level                        = du_low.loggers.phy_level;
  upper_phy_factory_config.enable_logging_broadcast         = du_low.loggers.broadcast_enabled;
  upper_phy_factory_config.logger_max_hex_size              = du_low.loggers.hex_max_size;
  upper_phy_factory_config.enable_metrics                   = du_low.metrics_cfg.enable_du_low;
  upper_phy_factory_config.pusch_sinr_calc_method =
      channel_state_information::sinr_type_from_string(du_low.expert_phy_cfg.pusch_sinr_calc_method);
  upper_phy_factory_config.rx_symbol_printer.filename = du_low.loggers.phy_rx_symbol_printer.filename;
  upper_phy_factory_config.rx_symbol_printer.ul_ports = {0, max_nof_rx_antennas};
  upper_phy_factory_config.rx_symbol_printer.triggers.prach_threshold_rssi_dB =
      du_low.loggers.phy_rx_symbol_printer.triggers.prach_threshold_rssi_dB;
  upper_phy_factory_config.rx_symbol_printer.triggers.pusch_on_ko =
      du_low.loggers.phy_rx_symbol_printer.triggers.pusch_on_ko;
  upper_phy_factory_config.rx_symbol_printer.triggers.pusch_threshold_sinr_dB =
      du_low.loggers.phy_rx_symbol_printer.triggers.pusch_threshold_sinr_dB;
  upper_phy_factory_config.ldpc_encoder_type          = "auto";
  // Effective backends of the uplink pipeline mode: resolved once above, so the factories only ever see a concrete
  // implementation (the LDPC decoder honors the expert knob when the Metal codec is built in, and stays on the CPU
  // default otherwise - it is not part of the fused lane).
  upper_phy_factory_config.ldpc_decoder_type          = effective.ldpc;
  upper_phy_factory_config.ldpc_decoder_offset        = du_low.expert_phy_cfg.ldpc_decoder_offset;
  upper_phy_factory_config.ldpc_rate_dematcher_type   = "auto";
  upper_phy_factory_config.crc_calculator_type        = "auto";
  upper_phy_factory_config.prach_th_correction_factor = du_low.expert_phy_cfg.prach_th_correction_factor;
  upper_phy_factory_config.pusch_channel_estimator_fd_strategy =
      du_low.expert_phy_cfg.pusch_channel_estimator_fd_strategy;
  upper_phy_factory_config.pusch_channel_estimator_td_strategy =
      du_low.expert_phy_cfg.pusch_channel_estimator_td_strategy;
  upper_phy_factory_config.pusch_channel_estimator_compensate_cfo =
      du_low.expert_phy_cfg.pusch_channel_estimator_cfo_compensation;
  upper_phy_factory_config.pusch_channel_estimator_algo = effective.ch_est;
  upper_phy_factory_config.pusch_channel_estimator_mmse_tau_rms_us =
      du_low.expert_phy_cfg.pusch_channel_estimator_mmse_tau_rms_us;
  upper_phy_factory_config.pusch_channel_estimator_mmse_fd_hz =
      du_low.expert_phy_cfg.pusch_channel_estimator_mmse_fd_hz;
  upper_phy_factory_config.pusch_channel_estimator_mmse_block_prb =
      du_low.expert_phy_cfg.pusch_channel_estimator_mmse_block_prb;
  upper_phy_factory_config.pusch_channel_estimator_helena_model_path =
      du_low.expert_phy_cfg.pusch_channel_estimator_helena_model_path;
  upper_phy_factory_config.pusch_channel_estimator_helena_model_path_52 =
      du_low.expert_phy_cfg.pusch_channel_estimator_helena_model_path_52;
  upper_phy_factory_config.pusch_channel_estimator_helena_model_path_106 =
      du_low.expert_phy_cfg.pusch_channel_estimator_helena_model_path_106;
  upper_phy_factory_config.pusch_channel_equalizer_algorithm = du_low.expert_phy_cfg.pusch_channel_equalizer_algorithm;
  upper_phy_factory_config.pusch_channel_equalizer_backend   = effective.equalizer;
  upper_phy_factory_config.ldpc_decoder_iterations           = du_low.expert_phy_cfg.pusch_decoder_max_iterations;
  upper_phy_factory_config.ldpc_decoder_early_stop           = du_low.expert_phy_cfg.pusch_decoder_early_stop;
  upper_phy_factory_config.ldpc_decoder_force_decoding       = du_low.expert_phy_cfg.pusch_decoder_force_decoding;
  upper_phy_factory_config.nof_rx_ports                      = max_nof_rx_antennas;
  upper_phy_factory_config.ul_bw_rb                          = max_ul_bw_rb;
  upper_phy_factory_config.pusch_max_nof_layers              = pusch_max_nof_layers;
  upper_phy_factory_config.enable_metrics                    = du_low.metrics_cfg.enable_du_low;
  if (du_low.loggers.phy_rx_symbol_printer.port.has_value()) {
    upper_phy_factory_config.rx_symbol_printer.ul_ports.set(*du_low.loggers.phy_rx_symbol_printer.port,
                                                            *du_low.loggers.phy_rx_symbol_printer.port + 1);
  }
  if (du_low.expert_phy_cfg.enable_phy_tap) {
    upper_phy_factory_config.phy_tap_arguments = du_low.expert_phy_cfg.phy_tap_arguments;
    if (cells[0].tdd_pattern) {
      upper_phy_factory_config.phy_tap_tdd_pattern = cells[0].tdd_pattern;
    }
  }

  // The flexible PDSCH processor implementation will be used by default.
  const auto& upper_phy_threads_cfg = du_low.expert_execution_cfg.threads;
  if ((upper_phy_threads_cfg.pdsch_processor_type == "auto") ||
      (upper_phy_threads_cfg.pdsch_processor_type == "flexible")) {
    // Set the batch size for synchronous operation by default. Override it if the PDSCH codeblock processor operation
    // is asynchronous.
    unsigned cb_batch_length = pdsch_processor_flexible_configuration::synchronous_cb_batch_length;
    if (upper_phy_threads_cfg.pdsch_cb_batch_length != du_low_unit_expert_threads_config::synchronous_cb_batch_length) {
      cb_batch_length = upper_phy_threads_cfg.pdsch_cb_batch_length;
    }

    // Emplace configuration parameters.
    upper_phy_factory_config.pdsch_processor.emplace<pdsch_processor_flexible_configuration>(
        pdsch_processor_flexible_configuration{.cb_batch_length = cb_batch_length});
  } else if (upper_phy_threads_cfg.pdsch_processor_type == "generic") {
    // The hardware-accelerated DU does not currently support the 'generic' PDSCH processor implementation.
    bool hw_acc_pdsch = false;
    if (du_low.hal_config.has_value()) {
      const du_low_unit_hal_config& hal_config = *du_low.hal_config;
      if (hal_config.bbdev_hwacc.has_value()) {
        const bbdev_appconfig& bbdev_hwacc = *hal_config.bbdev_hwacc;
        hw_acc_pdsch                       = bbdev_hwacc.pdsch_enc.has_value();
      }
    }
    report_error_if_not(!hw_acc_pdsch,
                        "The hardware-accelerated DU does not support the 'generic' PDSCH processor type.");
    upper_phy_factory_config.pdsch_processor.emplace<pdsch_processor_generic_configuration>();
  }

  for (unsigned i = 0, e = cells.size(); i != e; ++i) {
    const o_du_low_unit_config::du_low_config& cell           = cells[i];
    upper_phy_configuration&                   upper_phy_cell = out_config.cells.emplace_back().upper_phy_cfg;

    // Get bandwidth in PRB.
    const unsigned bw_rb = cell.bw_rb;
    // Deduce the number of slots per subframe.
    const unsigned nof_slots_per_subframe = get_nof_slots_per_subframe(cell.scs_common);
    // Deduce the number of slots per frame.
    unsigned nof_slots_per_frame = nof_slots_per_subframe * NOF_SUBFRAMES_PER_FRAME;
    // Number of slots per hyper system frame.
    unsigned nof_slots_per_hyper_system_frame = NOF_SFNS * nof_slots_per_frame;
    // PUSCH HARQ process lifetime in slots. It assumes the maximum lifetime is 100ms.
    unsigned expire_pusch_harq_timeout_slots =
        100 * nof_slots_per_subframe + du_low.expert_phy_cfg.max_processing_delay_slots;
    // In a NTN cell, extend the HARQ process lifetime by the cell-specific-k-offset.
    if (cell.ntn_cs_koffset) {
      expire_pusch_harq_timeout_slots += cell.ntn_cs_koffset->count() * nof_slots_per_subframe;
    }

    // Calculate the number of UL slots in a frame and in a PUSCH HARQ process lifetime.
    unsigned nof_ul_slots_in_harq_lifetime = expire_pusch_harq_timeout_slots;
    if (cell.duplex == duplex_mode::TDD && cell.tdd_pattern.has_value()) {
      const tdd_ul_dl_pattern& pattern1     = cell.tdd_pattern->pattern1;
      unsigned                 period_slots = pattern1.dl_ul_tx_period_nof_slots;
      unsigned                 nof_ul_slots = pattern1.nof_ul_slots + ((pattern1.nof_ul_symbols != 0) ? 1 : 0);
      if (cell.tdd_pattern->pattern2) {
        const tdd_ul_dl_pattern& pattern2 = *cell.tdd_pattern->pattern2;
        period_slots += pattern2.dl_ul_tx_period_nof_slots;
        nof_ul_slots += pattern2.nof_ul_slots + ((pattern2.nof_ul_symbols != 0) ? 1 : 0);
      }
      nof_ul_slots_in_harq_lifetime = divide_ceil(expire_pusch_harq_timeout_slots, period_slots) * nof_ul_slots;
    }

    // Deduce the maximum number of codeblocks that can be scheduled for PUSCH in one slot assuming:
    // - The maximum number of resource elements used for data for each scheduled resource block;
    // - the cell bandwidth;
    // - the highest modulation order possible; and
    // - the maximum coding rate.
    const unsigned max_nof_pusch_cb_slot =
        divide_ceil(pusch_constants::MAX_NRE_PER_RB * bw_rb * get_bits_per_symbol(modulation_scheme::QAM256),
                    ldpc::MAX_MESSAGE_SIZE);

    // Calculate the maximum number of active PUSCH HARQ processes from:
    // - the maximum number of users per slot; and
    // - the number of PUSCH occasions in a HARQ process lifetime.
    const unsigned nof_buffers = cell.max_puschs_per_slot * nof_ul_slots_in_harq_lifetime;

    // Calculate the maximum number of receive codeblocks. It is equal to the product of:
    // - the maximum number of codeblocks that can be scheduled in one slot; and
    // - the number of PUSCH occasions in a HARQ process lifetime.
    const unsigned max_rx_nof_codeblocks = nof_ul_slots_in_harq_lifetime * max_nof_pusch_cb_slot;

    // Determine processing downlink pipeline depth. Make sure the number of slots per system frame is divisible by the
    // pipeline depth.
    unsigned dl_pipeline_depth = 4 * du_low.expert_phy_cfg.max_processing_delay_slots;
    while (nof_slots_per_hyper_system_frame % dl_pipeline_depth != 0) {
      ++dl_pipeline_depth;
    }

    // Slots per frame, times the slack the uplink chain needs per processor.
    //
    // One slot per frame was the historical choice ("reusing uplink processors every 10 ms"): a processor is
    // handed a new uplink slot only after the previous one's PDU tasks have all finished, so this number is
    // the slack between "the task of slot N is still running" and "slot N + depth arrives". The D1 block
    // hand-over moves the resource grid's production to the LANE's commit, so a hop's task outlives its slot
    // by a little more than it used to, and at the offered-load ramp the chain then refused the next UL_TTI
    // for that processor - `Real-time failure in FAPI: UL processor is busy`, measured 716 against the
    // control arm's 84 in the same pair, which drops that slot's grants altogether (design document 5.9.25).
    //
    // Three frames of slack (30 ms at 15 kHz) took that from 716 refusals to 91 and the control arm's 84 to
    // 0 (design document 5.9.27). The 91 that were left sit in the same few seconds - the offered load
    // stepping to one full-bandwidth grant per slot - so this is the SECOND notch: six frames, 60 ms, which
    // is two orders of magnitude above a task's own lifetime (lane + LDPC ~1-2 ms) and leaves the step's
    // queueing burst room to drain.
    //
    // The price is one resource grid plus a payload pool per extra processor (~34 KB per grid at 25 PRB,
    // ~140 KB at 51 PRB with two ports), and the number of CPU submissions per slot does NOT change with it:
    // the hand-over still commits one block per received slot. The DURABLE fix is not this number - it is to
    // stop serializing a processor's slots behind the previous slot's tasks (design document 5.9.28), and
    // this notch is what keeps the deadline off the table until that lands.
    unsigned ul_pipeline_depth = 6 * nof_slots_per_frame;

    const prach_configuration prach_cfg =
        prach_configuration_get(cell.freq_range, cell.duplex, cell.prach_config_index);
    ocudu_assert(prach_cfg.format != prach_format_type::invalid,
                 "Unsupported PRACH configuration index (i.e., {}) for the given frequency range (i.e., {}) and "
                 "duplex mode (i.e., {}).",
                 cell.prach_config_index,
                 to_string(cell.freq_range),
                 to_string(cell.duplex));

    upper_phy_cell.sector                     = i;
    upper_phy_cell.nof_tx_ports               = cell.nof_tx_antennas;
    upper_phy_cell.nof_rx_ports               = cell.nof_rx_antennas;
    upper_phy_cell.nof_dl_rg                  = dl_pipeline_depth + 2;
    upper_phy_cell.nof_ul_rg                  = ul_pipeline_depth;
    upper_phy_cell.nof_prach_buffer           = du_low.expert_phy_cfg.max_processing_delay_slots + 2;
    upper_phy_cell.max_nof_td_prach_occasions = prach_cfg.nof_occasions_within_slot;
    upper_phy_cell.max_nof_fd_prach_occasions = 1;
    upper_phy_cell.is_prach_long_format       = is_long_preamble(prach_cfg.format);
    upper_phy_cell.nof_dl_processors          = dl_pipeline_depth;
    upper_phy_cell.dl_bw_rb                   = bw_rb;
    upper_phy_cell.ul_bw_rb                   = bw_rb;
    upper_phy_cell.pusch_max_nof_layers       = cell.pusch_max_nof_layers;
    upper_phy_cell.active_scs                 = {};
    upper_phy_cell.active_scs[to_numerology_value(cell.scs_common)] = true;
    upper_phy_cell.rx_buffer_config.nof_buffers                     = nof_buffers;
    upper_phy_cell.rx_buffer_config.nof_codeblocks                  = max_rx_nof_codeblocks;
    upper_phy_cell.rx_buffer_config.max_codeblock_size              = ldpc::MAX_CODEBLOCK_SIZE;
    upper_phy_cell.rx_buffer_config.expire_timeout_slots            = expire_pusch_harq_timeout_slots;
    upper_phy_cell.rx_buffer_config.external_soft_bits              = false;

    if (!is_valid_upper_phy_config(upper_phy_cell)) {
      report_error("Invalid upper PHY configuration.\n");
    }
  }

  return out_config;
}

void ocudu::generate_o_du_low_config(odu::o_du_low_config&                           out_config,
                                     const du_low_unit_config&                       du_low_unit_cfg,
                                     span<const o_du_low_unit_config::du_low_config> cells)
{
  out_config.du_low_cfg = generate_du_low_config(du_low_unit_cfg, cells);
}

/// \brief Derives the required PUSCH and SRS concurrency level from the per-cell parameters.
///
/// Applies the formula: 12.5 cores for every 100MHz, layer and cell, scaled by the cell's UL slot ratio, aggregated
/// across all cells, since PUSCH and SRS executors are shared globally rather than partitioned per cell. Unspecified
/// fields use sensible defaults (1 antenna, 100MHz, 1 layer, FDD i.e. UL ratio of 1).
static unsigned derive_pusch_and_srs_concurrency(span<const du_low_cell_config> cell_params)
{
  report_error_if_not(!cell_params.empty(),
                      "At least one cell must be configured before auto-deriving PUSCH/SRS concurrency.");

  // Number of CPU cores required per 100MHz of channel bandwidth, per PUSCH layer, per cell (at 2 LDPC iterations).
  constexpr double cores_per_100mhz_layer_cell = 12.5;

  unsigned nof_available_cpus = cpu_architecture_info::get().get_host_nof_available_cpus();

  double required = 0.0;
  for (const auto& c : cell_params) {
    required += cores_per_100mhz_layer_cell * static_cast<double>(c.channel_bw_mhz) / 100.0 *
                static_cast<double>(c.pusch_max_nof_layers) * c.ul_ratio;
  }

  unsigned required_concurrency = std::max(1U, static_cast<unsigned>(std::ceil(required)));

  // A requirement that meets or exceeds the number of available cores is equivalent to no limitation.
  if (required_concurrency >= nof_available_cpus) {
    return du_low_unit_expert_threads_config::concurrency_unlimited;
  }
  return required_concurrency;
}

void ocudu::fill_du_low_worker_manager_config(worker_manager_config&         config,
                                              const du_low_unit_config&      unit_cfg,
                                              unsigned                       is_blocking_mode_active,
                                              span<const du_low_cell_config> cell_params)
{
  auto& du_low_cfg = config.du_low_cfg.emplace();

  du_low_cfg.is_sequential_mode_active = is_blocking_mode_active;

  du_low_cfg.cell_nof_dl_antennas.reserve(cell_params.size());
  du_low_cfg.cell_nof_ul_antennas.reserve(cell_params.size());
  for (const auto& c : cell_params) {
    du_low_cfg.cell_nof_dl_antennas.push_back(c.nof_dl_antennas);
    du_low_cfg.cell_nof_ul_antennas.push_back(c.nof_ul_antennas);
  }

  unsigned max_pdsch_concurrency         = unit_cfg.expert_execution_cfg.threads.max_pdsch_concurrency;
  unsigned max_pusch_and_srs_concurrency = unit_cfg.expert_execution_cfg.threads.max_pusch_and_srs_concurrency;

  if (max_pusch_and_srs_concurrency == du_low_unit_expert_threads_config::concurrency_auto) {
    max_pusch_and_srs_concurrency = derive_pusch_and_srs_concurrency(cell_params);
  }

  // Override PDSCH and PUSCH maximum concurrency if hardware acceleration is present.
  if (unit_cfg.hal_config.has_value()) {
    const du_low_unit_hal_config& hal_config = *unit_cfg.hal_config;
    if (hal_config.bbdev_hwacc.has_value()) {
      const bbdev_appconfig& bbdev_hwacc = *hal_config.bbdev_hwacc;
      if (bbdev_hwacc.pdsch_enc.has_value()) {
        const hwacc_pdsch_appconfig& pdsch_enc = *bbdev_hwacc.pdsch_enc;
        if (max_pdsch_concurrency != pdsch_enc.nof_hwacc) {
          fmt::print("Warning: the configured maximum PDSCH concurrency ({}) is overridden by the number of PDSCH "
                     "encoder hardware accelerated functions ({})\n",
                     max_pdsch_concurrency,
                     pdsch_enc.nof_hwacc);
          max_pdsch_concurrency = pdsch_enc.nof_hwacc;
        }
      }
      if (bbdev_hwacc.pusch_dec.has_value()) {
        const hwacc_pusch_appconfig& pusch_dec = *bbdev_hwacc.pusch_dec;
        if (max_pusch_and_srs_concurrency != pusch_dec.nof_hwacc) {
          fmt::print("Warning: the configured maximum PUSCH and SRS concurrency ({}) is overridden by the number of "
                     "PUSCH decoder hardware accelerated functions ({})\n",
                     max_pusch_and_srs_concurrency,
                     pusch_dec.nof_hwacc);
          max_pusch_and_srs_concurrency = pusch_dec.nof_hwacc;
        }
      }
    }
  }

  // ---- P0-6 (doc_chinese/phy_latency/01_plan.md): say what this value RESOLVED to, and from what ----
  // The resolved concurrency decides whether the PUSCH lane's executor is a serialising STRAND (<= 1,
  // one hop at a time) or an N-way task fork limiter (N hops may overlap). Until this line existed the
  // value was nowhere in a leg: the config default is `auto`, the YAML dump writes the sentinel back
  // out, and the only readings that ever named a "single lane" came from n1 legs (5.9.33/5.9.35). So
  // whether the n78 heavy leg's 901us channel-estimation segment may be attributed to single-lane
  // queueing was UNANSWERABLE from the record - and the two configurations need not agree, because
  // derive_pusch_and_srs_concurrency() scales with bandwidth and (for TDD) with the uplink slot ratio.
  // Printed to stderr, the stream the wip/ gates read a leg's counters from.
  {
    std::string inputs;
    for (const du_low_cell_config& c : cell_params) {
      inputs += fmt::format(
          " bw={}MHz layers={} ul_ratio={:.2f};", c.channel_bw_mhz, c.pusch_max_nof_layers, c.ul_ratio);
    }
    std::fprintf(stderr,
                 "[ul_lane_exec] PUSCH/SRS concurrency = %u (%s;%s available cpus=%u)"
                 "  <- <=1 makes the lane a serialising strand, else an N-way fork limiter"
                 " (see du_low_executor_mapper.cpp)\n",
                 max_pusch_and_srs_concurrency,
                 (unit_cfg.expert_execution_cfg.threads.max_pusch_and_srs_concurrency ==
                  du_low_unit_expert_threads_config::concurrency_auto)
                     ? "auto-derived"
                     : ((max_pusch_and_srs_concurrency == du_low_unit_expert_threads_config::concurrency_unlimited)
                            ? "configured: no limit"
                            : "configured"),
                 inputs.c_str(),
                 static_cast<unsigned>(cpu_architecture_info::get().get_host_nof_available_cpus()));
  }

  du_low_cfg.max_pdsch_concurrency         = max_pdsch_concurrency;
  du_low_cfg.max_pucch_concurrency         = unit_cfg.expert_execution_cfg.threads.max_pucch_concurrency;
  du_low_cfg.max_pusch_and_srs_concurrency = max_pusch_and_srs_concurrency;
  du_low_cfg.executor_tracing_enable       = unit_cfg.tracer.executor_tracing_enable;
}
