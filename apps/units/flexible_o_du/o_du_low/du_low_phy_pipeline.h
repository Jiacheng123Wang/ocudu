// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/phy/phy_pipeline_mode.h"
#include "ocudu/support/error_handling.h"
#include <optional>
#include <string>
#include <string_view>

namespace ocudu {

/// Expert-phy backend knobs that take part in the uplink pipeline orchestration, plus the pipeline mode itself.
///
/// All of them carry their raw command-line values, "auto" included: "auto" is only resolved here.
struct phy_pipeline_request {
  /// `expert_phy --phy_pipeline`: "auto" (derive the mode from the module knobs), "cpu", "cpu_gpu" or "gpu".
  std::string mode = "auto";
  /// `expert_phy --pusch_dft_type`.
  std::string dft = "auto";
  /// `expert_phy --pusch_channel_estimator_algo`.
  std::string ch_est = "auto";
  /// `expert_phy --pusch_channel_equalizer_backend`.
  std::string equalizer = "auto";
  /// `expert_phy --pusch_ldpc_decoder_type`.
  std::string ldpc = "auto";
  /// `expert_phy --device_resource_grid`: "auto" (follow the pipeline mode), "on" or "off".
  std::string device_grid = "auto";
};

/// Which offload backends are linked into this binary (from the ENABLE_METAL_* build options).
struct phy_backend_availability {
  bool dft       = false;
  bool ch_est    = false;
  bool equalizer = false;
  bool demapper  = false;
  bool ldpc      = false;
};

/// Availability of the offload backends in this binary.
///
/// \note Deliberately not inline: the value comes from build-time definitions that only this component's translation
/// units carry, so every TU must link the very same definition (see du_low_phy_pipeline.cpp) instead of compiling its
/// own copy - callers outside this component (e.g. the flexible O-DU factory, which resolves the DFT backend for the
/// lower PHY) would otherwise silently take an all-unavailable copy.
phy_backend_availability query_phy_backend_availability();

/// Effective configuration of the uplink pipeline: one backend per module, all "auto" resolved.
struct phy_pipeline_effective {
  /// Effective mode (never "auto").
  phy_pipeline_mode mode = phy_pipeline_mode::cpu;
  /// Effective DFT backend: "cpu" or "metal".
  std::string dft = "cpu";
  /// Effective channel estimator backend: "cpu", "metal_mmse", "metal_nn_mmse" or "helena".
  std::string ch_est = "cpu";
  /// Effective channel equalizer backend: "cpu" or "metal".
  std::string equalizer = "cpu";
  /// Effective LDPC decoder backend (the LDPC decoder is not part of the fused lane).
  std::string ldpc = "auto";
  /// True when the mode runs the IQ -> LLR chain as one fused device-side lane.
  bool lane_fused = false;
  /// Keep the resource grid on the device: the OFDM demodulation writes it from the GPU (see
  /// ofdm_demodulator_configuration::device_grid_write) and the CPU reads the same memory.
  bool device_grid = false;
};

/// Whether a per-module backend value runs on the CPU.
///
/// "auto" counts as CPU: outside the fused lane it selects the CPU implementation, which is the historical default of
/// every one of these knobs. The LDPC-specific CPU flavors ("generic", "neon", "avx2", "avx512") are CPU as well;
/// everything else ("metal", "metal_*", "helena") offloads.
inline bool is_cpu_phy_backend(std::string_view value)
{
  return (value == "auto") || (value == "cpu") || (value == "generic") || (value == "neon") || (value == "avx2") ||
         (value == "avx512");
}

/// Per-module backend substitutions applied when the fused lane takes a module over.
namespace phy_pipeline_lane_defaults {
/// DFT backend of the fused lane.
constexpr const char* dft = "metal";
/// Channel estimator backend of the fused lane.
constexpr const char* ch_est = "metal_mmse";
/// Channel equalizer backend of the fused lane.
constexpr const char* equalizer = "metal";
} // namespace phy_pipeline_lane_defaults

/// Appends a "option conflicts with the mode" message to \c error.
inline void set_phy_pipeline_conflict(std::string& error, phy_pipeline_mode mode, std::string_view option, std::string_view value)
{
  error = "--phy_pipeline " + std::string(to_string(mode)) + " conflicts with " + std::string(option) + " " +
          std::string(value);
  if (mode == phy_pipeline_mode::cpu) {
    error += ": the CPU pipeline requires a CPU backend for every module (use --phy_pipeline cpu_gpu to offload "
             "individual modules)";
  } else {
    error += ": the fused lane runs the whole chain on the device, so the backend of the modules it owns is selected "
             "by the pipeline mode (leave them at their default \"auto\")";
  }
}

/// \brief Resolves the effective uplink pipeline configuration from the expert-phy knobs.
///
/// Three rules, one per mode (see phy_pipeline_mode):
/// - \c cpu: every module knob must be a CPU backend, otherwise it is a configuration conflict. "auto" resolves to
///   "cpu".
/// - \c cpu_gpu: the knobs are honored as configured; "auto" resolves to "cpu".
/// - \c gpu: the fused lane takes over the DFT, the channel estimator, the equalizer and the demapper. Their knobs
///   must be left at "auto" (an explicit CPU backend is a conflict: this mode has no CPU fallback); "auto" resolves
///   to the lane's own backend. The LDPC decoder is not part of the lane, so its knob keeps its meaning in every
///   mode -- the LLR still leaves the device for the CPU decoder.
///
/// A backend that is not built into this binary (ENABLE_METAL_* off) falls back to its CPU implementation, which is
/// the same policy the PHY factories apply at construction time; the caller logs the substitution as part of the
/// effective configuration.
///
/// \param[in]  request   Raw expert-phy knobs.
/// \param[in]  available Offload backends built into this binary.
/// \param[out] error     Human-readable reason when the function returns an empty optional.
/// \return The effective configuration, or an empty optional on a configuration conflict.
inline std::optional<phy_pipeline_effective>
resolve_phy_pipeline(const phy_pipeline_request& request, const phy_backend_availability& available, std::string& error)
{
  phy_pipeline_effective out;
  out.dft       = request.dft;
  out.ch_est    = request.ch_est;
  out.equalizer = request.equalizer;
  out.ldpc      = request.ldpc;

  // Effective mode: an unspecified mode follows the module knobs, so that a command line without --phy_pipeline keeps
  // behaving exactly as it did before the mode existed.
  if (request.mode == "auto") {
    const bool offload = !is_cpu_phy_backend(request.dft) || !is_cpu_phy_backend(request.ch_est) ||
                         !is_cpu_phy_backend(request.equalizer) || !is_cpu_phy_backend(request.ldpc);
    out.mode = offload ? phy_pipeline_mode::cpu_gpu : phy_pipeline_mode::cpu;
  } else if (!phy_pipeline_mode_from_string(request.mode, out.mode)) {
    error = "Invalid UL PHY pipeline mode '" + request.mode + "'. Accepted values [auto,cpu,cpu_gpu,gpu]";
    return std::nullopt;
  }

  // Device resource grid: "auto" follows the mode (the fused lane owns the grid, the module-level offload keeps the
  // host one, so a command line without this knob behaves exactly as before the capability existed).
  if (request.device_grid == "auto") {
    out.device_grid = (out.mode == phy_pipeline_mode::gpu);
  } else if (request.device_grid == "on") {
    out.device_grid = true;
  } else if (request.device_grid == "off") {
    out.device_grid = false;
  } else {
    error = "Invalid device resource grid value '" + request.device_grid + "'. Accepted values [auto,on,off]";
    return std::nullopt;
  }

  switch (out.mode) {
    case phy_pipeline_mode::cpu:
      // No module may offload in this mode: report the first conflict instead of silently running on the CPU.
      if (!is_cpu_phy_backend(request.dft)) {
        set_phy_pipeline_conflict(error, out.mode, "--pusch_dft_type", request.dft);
        return std::nullopt;
      }
      if (!is_cpu_phy_backend(request.ch_est)) {
        set_phy_pipeline_conflict(error, out.mode, "--pusch_channel_estimator_algo", request.ch_est);
        return std::nullopt;
      }
      if (!is_cpu_phy_backend(request.equalizer)) {
        set_phy_pipeline_conflict(error, out.mode, "--pusch_channel_equalizer_backend", request.equalizer);
        return std::nullopt;
      }
      if (!is_cpu_phy_backend(request.ldpc)) {
        set_phy_pipeline_conflict(error, out.mode, "--pusch_ldpc_decoder_type", request.ldpc);
        return std::nullopt;
      }
      // Writing the grid from the device is an offload of the OFDM demodulation, which this mode does not allow.
      if (request.device_grid == "on") {
        set_phy_pipeline_conflict(error, out.mode, "--device_resource_grid", request.device_grid);
        return std::nullopt;
      }
      // Keep the requested CPU flavor of the LDPC decoder (e.g. a specific SIMD implementation).
      out.dft       = "cpu";
      out.ch_est    = "cpu";
      out.equalizer = "cpu";
      break;

    case phy_pipeline_mode::cpu_gpu:
      if (request.dft == "auto") {
        out.dft = "cpu";
      }
      if (request.ch_est == "auto") {
        out.ch_est = "cpu";
      }
      if (request.equalizer == "auto") {
        out.equalizer = "cpu";
      }
      break;

    case phy_pipeline_mode::gpu:
      // The lane owns these four modules: an explicit CPU backend cannot be honored, so it is a conflict rather than
      // a silent override.
      if (request.dft == "cpu") {
        set_phy_pipeline_conflict(error, out.mode, "--pusch_dft_type", request.dft);
        return std::nullopt;
      }
      if (request.ch_est == "cpu") {
        set_phy_pipeline_conflict(error, out.mode, "--pusch_channel_estimator_algo", request.ch_est);
        return std::nullopt;
      }
      if (request.equalizer == "cpu") {
        set_phy_pipeline_conflict(error, out.mode, "--pusch_channel_equalizer_backend", request.equalizer);
        return std::nullopt;
      }
      out.dft       = (request.dft == "auto") ? phy_pipeline_lane_defaults::dft : request.dft;
      out.ch_est    = (request.ch_est == "auto") ? phy_pipeline_lane_defaults::ch_est : request.ch_est;
      out.equalizer = (request.equalizer == "auto") ? phy_pipeline_lane_defaults::equalizer : request.equalizer;
      out.lane_fused = true;
      break;
  }

  // A backend that is not linked into this binary falls back to the CPU implementation (or, for the LDPC decoder, to
  // the CPU-selecting "auto") instead of failing the whole application. The caller reports the substitution.
  if (!available.dft && !is_cpu_phy_backend(out.dft)) {
    out.dft = "cpu";
  }
  if (!available.ch_est && !is_cpu_phy_backend(out.ch_est)) {
    out.ch_est = "cpu";
  }
  if (!available.equalizer && !is_cpu_phy_backend(out.equalizer)) {
    out.equalizer = "cpu";
  }
  if (!available.ldpc && !is_cpu_phy_backend(out.ldpc)) {
    out.ldpc = "auto";
  }

  return out;
}

/// Checks the prerequisites of the fused lane: the offload backends it is built from must be linked in.
/// \return An empty string when the lane can run, the reason otherwise.
inline std::string check_phy_pipeline_lane_available(const phy_backend_availability& available)
{
  if (available.dft && available.ch_est && available.equalizer && available.demapper) {
    return {};
  }
  return "the fused UL PHY pipeline (--phy_pipeline gpu) requires the Metal DFT, channel estimator, equalizer and "
         "soft demapper backends, which are not built into this binary";
}

/// \brief Resolves the effective configuration from the expert-phy knobs, aborting on a configuration conflict.
///
/// The configuration validator rejects conflicts before any factory is built; reaching this point with one means the
/// two paths disagree, so fail loudly instead of silently running a pipeline the user did not ask for.
inline phy_pipeline_effective resolve_phy_pipeline_or_fatal(std::string_view mode,
                                                            std::string_view dft,
                                                            std::string_view ch_est,
                                                            std::string_view equalizer,
                                                            std::string_view ldpc,
                                                            std::string_view device_grid)
{
  const phy_pipeline_request request{std::string(mode),
                                     std::string(dft),
                                     std::string(ch_est),
                                     std::string(equalizer),
                                     std::string(ldpc),
                                     std::string(device_grid)};
  std::string                error;
  std::optional<phy_pipeline_effective> resolved =
      resolve_phy_pipeline(request, query_phy_backend_availability(), error);
  if (!resolved.has_value()) {
    report_fatal_error("Invalid UL PHY pipeline configuration: {}", error);
  }
  return *resolved;
}

/// \brief Resolves the effective configuration of the given expert-phy knobs (aborting on a conflict).
///
/// Both sides of the lower PHY / upper PHY boundary resolve their backends through this single entry point, so they
/// cannot drift apart (the DFT backend is consumed by the lower PHY, through the radio-unit configuration).
template <typename ExpertPhyConfig>
phy_pipeline_effective resolve_phy_pipeline_or_fatal(const ExpertPhyConfig& config)
{
  return resolve_phy_pipeline_or_fatal(config.phy_pipeline,
                                       config.pusch_dft_type,
                                       config.pusch_channel_estimator_algo,
                                       config.pusch_channel_equalizer_backend,
                                       config.ldpc_decoder_type,
                                       config.device_resource_grid);
}

} // namespace ocudu
