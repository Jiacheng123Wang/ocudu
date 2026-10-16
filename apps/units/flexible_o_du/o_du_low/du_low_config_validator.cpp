// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "du_low_config_validator.h"
#include "du_low_phy_pipeline.h"
#include "ocudu/adt/format.h"
#include "ocudu/adt/interval.h"
#include "ocudu/phy/upper/channel_processors/prach/prach_detector_phy_validator.h"
#include <filesystem>
#include <thread>

using namespace ocudu;

static bool validate_upper_phy_threads_appconfig(const du_low_unit_expert_threads_config& config)
{
  static const interval<unsigned, true> max_concurrency_range(0, std::thread::hardware_concurrency());

  bool valid = true;

  if (!max_concurrency_range.contains(config.max_pucch_concurrency)) {
    fmt::print("Maximum PUCCH concurrency (i.e., {}) must be in range {}.\n",
               config.max_pucch_concurrency,
               max_concurrency_range);
    valid = false;
  }

  if ((config.max_pusch_and_srs_concurrency != du_low_unit_expert_threads_config::concurrency_auto) &&
      !max_concurrency_range.contains(config.max_pusch_and_srs_concurrency)) {
    fmt::print("Maximum joint PUSCH and SRS concurrency (i.e., {}) must be in range {}.\n",
               config.max_pusch_and_srs_concurrency,
               max_concurrency_range);
    valid = false;
  }

  if ((config.pdsch_processor_type != "auto") && (config.pdsch_processor_type != "flexible") &&
      (config.pdsch_processor_type != "generic")) {
    fmt::print("Invalid PDSCH processor type. Valid types are: auto, generic and flexible.\n");
    valid = false;
  }

  if (!max_concurrency_range.contains(config.max_pdsch_concurrency)) {
    fmt::print("Maximum PDSCH concurrency (i.e., {}) must be in range {}.\n",
               config.max_pdsch_concurrency,
               max_concurrency_range);
    valid = false;
  }

  return valid;
}

static bool validate_expert_execution_unit_config(const du_low_unit_config& config)
{
  return validate_upper_phy_threads_appconfig(config.expert_execution_cfg.threads);
}

static bool validate_phy_prach_configuration(span<const du_low_prach_validation_config>& prach_cells_config)
{
  for (const auto& prach_cell_cfg : prach_cells_config) {
    error_type<std::string> valid_prach = validate_prach_detector_phy(prach_cell_cfg.format,
                                                                      prach_cell_cfg.prach_scs,
                                                                      prach_cell_cfg.zero_correlation_zone,
                                                                      prach_cell_cfg.nof_prach_ports);
    if (!valid_prach.has_value()) {
      fmt::print("Invalid configuration:\n    {}", valid_prach.error());
      return false;
    }
  }

  return true;
}

/// Validates the uplink PHY pipeline mode against the per-module backend knobs.
static bool validate_phy_pipeline_config(const du_low_unit_expert_upper_phy_config& config)
{
  phy_pipeline_request request;
  request.mode      = config.phy_pipeline;
  request.dft       = config.pusch_dft_type;
  request.ch_est    = config.pusch_channel_estimator_algo;
  request.equalizer = config.pusch_channel_equalizer_backend;
  request.ldpc      = config.ldpc_decoder_type;
  request.device_grid    = config.device_resource_grid;
  request.receiver       = config.pusch_receiver_backend;
  request.receiver_model = config.pusch_receiver_model;

  const phy_backend_availability available = query_phy_backend_availability();

  if (config.phy_pipeline == "gpu") {
    // The fused lane is built from the device-side chain: without those backends there is nothing to run.
    // The DFT is the exception, because the lane's DEFAULT for it is the CPU (the host writes the grid, see
    // du_low_phy_pipeline.h): "auto" resolves to a CPU DFT, so a binary without the Metal one can still run
    // this mode - and the Metal DFT is only required when the command line asks for it.
    const bool        dft_on_cpu = is_cpu_phy_backend(request.dft);
    const std::string lane_error = check_phy_pipeline_lane_available(available, dft_on_cpu);
    if (!lane_error.empty()) {
      fmt::print("Invalid configuration: {}.\n", lane_error);
      return false;
    }
    // The mode used to be refused here with "not implemented yet", so that the module-level path could
    // not run under a name promising a fused one. That refusal is gone. It is not what enforced the
    // claim anyway: the mode resolves to the very backends the module-level legs already pass
    // explicitly (see phy_pipeline_lane_defaults), so selecting it changes the ORCHESTRATION claim and
    // nothing else - and a claim is verified by measuring it, not by refusing to run.
    //
    // \note The refusal was also why the remaining gap could not be measured: with the mode
    // unavailable, no leg could state the property it promises ("only two host <-> device data
    // crossings, the IQ upload and the LLR download" - see phy_pipeline_mode.h), so the runtime
    // contract had nothing to check in that mode. The contract is what enforces the claim now.
  }

  // Cross-check the mode against the module backend knobs (see resolve_phy_pipeline for the rules). The resolution
  // also decides WHICH model an "ai" run binds - the requested one, or the build's default when the knob is absent -
  // so the existence check below asks about the model the run will actually use, and names it.
  std::string error;
  const auto  effective = resolve_phy_pipeline(request, available, error);
  if (!effective.has_value()) {
    fmt::print("Invalid configuration: {}.\n", error);
    return false;
  }

  // A model path that does not exist can never bind, and the run would say so only after the radio was opened --
  // as "AI ARM INACTIVE" plus a corpus whose labels came from the classical chain. It is decidable here, before
  // anything is built, so it is refused here. Whether a model that DOES exist binds is a runtime outcome (the
  // bundle may be an uncompiled .mlpackage, or of a shape this build cannot run) and stays reported by the arm.
  //
  // The error_code overload keeps a filesystem that cannot answer (a permission, an encoding this platform rejects)
  // from throwing here and from being read as "does not exist": only a check that RAN and said no is a refusal.
  if (effective->receiver == "ai") {
    std::error_code model_ec;
    const bool      model_exists = std::filesystem::exists(effective->receiver_model, model_ec);
    if (!model_exists && !model_ec) {
      fmt::print("Invalid configuration: the AI receiver's model '{}' does not exist{}. Pass the COMPILED model "
                 "directory (.mlmodelc) produced by `xcrun coremlcompiler compile`.\n",
                 effective->receiver_model,
                 config.pusch_receiver_model.empty() ? " (this is the build's default: --expert_phy."
                                                       "pusch_receiver_model was not given)"
                                                     : "");
      return false;
    }
  }

  return true;
}

/// Validates expert physical layer configuration parameters.
static bool validate_expert_phy_unit_config(const du_low_unit_expert_upper_phy_config& config)
{
  bool valid = true;

  if (!validate_phy_pipeline_config(config)) {
    valid = false;
  }

  if ((config.pusch_sinr_calc_method != "channel_estimator") &&
      (config.pusch_sinr_calc_method != "post_equalization") && (config.pusch_sinr_calc_method != "evm")) {
    fmt::print(
        "Invalid PUSCH SINR calculation method. Valid types are: channel_estimator, post_equalization and evm.\n");
    valid = false;
  }

  if (config.pusch_decoder_max_iterations == 0) {
    fmt::print("Maximum PUSCH LDPC decoder iterations cannot be zero.\n");
    valid = false;
  }

  if ((config.ldpc_decoder_type != "auto") && (config.ldpc_decoder_type != "generic") &&
      (config.ldpc_decoder_type != "neon") && (config.ldpc_decoder_type != "avx2") &&
      (config.ldpc_decoder_type != "avx512") && (config.ldpc_decoder_type != "metal") &&
      (config.ldpc_decoder_type != "metal_flooding") && (config.ldpc_decoder_type != "metal_persistent") &&
      (config.ldpc_decoder_type != "metal_async") && (config.ldpc_decoder_type != "metal_lls")) {
    fmt::print(
        "Invalid PUSCH LDPC decoder type. Valid types are: auto, generic, neon, avx2, avx512, metal, metal_flooding, "
        "metal_persistent, metal_async and metal_lls.\n");
    valid = false;
  }

  return valid;
}

static bool validate_log_options(const du_low_unit_logger_config& config, const du_low_prach_validation_config& cell)
{
  if (!config.phy_rx_symbol_printer.filename.empty() && config.phy_rx_symbol_printer.port.has_value() &&
      (config.phy_rx_symbol_printer.port.value() >= cell.nof_antennas_ul)) {
    fmt::print("Requested IQ dump from Rx port {}, valid Rx ports are 0-{}.\n",
               config.phy_rx_symbol_printer.port.value(),
               cell.nof_antennas_ul - 1);
    return false;
  }

  return true;
}

bool ocudu::validate_du_low_config(const du_low_unit_config&                  config,
                                   span<const du_low_prach_validation_config> prach_cells_config)
{
  if (!validate_log_options(config.loggers, prach_cells_config.front())) {
    return false;
  }

  if (!validate_expert_phy_unit_config(config.expert_phy_cfg)) {
    return false;
  }

  if (!validate_expert_execution_unit_config(config)) {
    return false;
  }

  if (!validate_phy_prach_configuration(prach_cells_config)) {
    return false;
  }

  return true;
}
