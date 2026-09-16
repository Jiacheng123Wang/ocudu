// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include <atomic>
#include <string_view>

namespace ocudu {

/// \brief Uplink PHY pipeline mode, selected with `expert_phy --phy_pipeline`.
///
/// The mode says how the uplink receive chain is *orchestrated*, which is orthogonal to the per-module backend knobs
/// (those say which implementation each module uses):
/// - \c cpu: the whole chain runs on the CPU. Every per-module backend knob must resolve to a CPU implementation; a
///   knob that asks for an offload backend is a configuration conflict (never silently ignored).
/// - \c cpu_gpu: module-level offload. Each module follows its own backend knob, so every module boundary keeps its own
///   host <-> device crossing.
/// - \c gpu: fused lane. The whole IQ -> LLR chain runs inside one device-side pipeline with only two host <-> device
///   *data* crossings (the IQ upload and the LLR download). The lane takes over the backends of the modules it
///   contains; the LDPC decoder is \b not part of the lane (the LLR still leaves the device for the CPU decoder).
enum class phy_pipeline_mode { cpu, cpu_gpu, gpu };

/// String form of a concrete mode, as used by the command line and the logs.
constexpr const char* to_string(phy_pipeline_mode mode)
{
  switch (mode) {
    case phy_pipeline_mode::cpu:
      return "cpu";
    case phy_pipeline_mode::cpu_gpu:
      return "cpu_gpu";
    case phy_pipeline_mode::gpu:
      return "gpu";
  }
  return "unknown";
}

/// \brief Parses one of the concrete modes.
///
/// \return True when \c value is a concrete mode. "auto" (and any other value) returns false: "auto" is a
/// command-line-only value that the application resolves from the module backend knobs.
constexpr bool phy_pipeline_mode_from_string(std::string_view value, phy_pipeline_mode& mode)
{
  if (value == "cpu") {
    mode = phy_pipeline_mode::cpu;
    return true;
  }
  if (value == "cpu_gpu") {
    mode = phy_pipeline_mode::cpu_gpu;
    return true;
  }
  if (value == "gpu") {
    mode = phy_pipeline_mode::gpu;
    return true;
  }
  return false;
}

/// \brief Process-wide effective pipeline mode, published for the instrumentation probes.
///
/// The probes are process-wide singletons without a configuration context of their own (see ul_pipeline_probe), yet
/// some of them only make sense in a given mode: the per-segment UL latencies measure the CPU side of the module
/// boundaries, which the fused lane removes altogether. Publishing the effective mode once at startup lets them switch
/// themselves off instead of reporting numbers that no longer mean what their name says.
///
/// \note This is a probe aid, not a data-path input: the data path always takes the mode from its own configuration.
class phy_pipeline_mode_registry
{
public:
  /// Publishes the effective mode (call once at startup, before any PHY thread runs).
  static void set(phy_pipeline_mode mode)
  {
    instance().store(mode, std::memory_order_relaxed);
    published().store(true, std::memory_order_relaxed);
  }

  /// \brief Whether an application published a mode.
  ///
  /// False in the unit tests and the tools, which run PHY components directly: a requirement that
  /// depends on the mode (see phy_pipeline_contract.h) has nothing to check there and must say so
  /// rather than judge a pipeline nobody selected.
  static bool is_published() { return published().load(std::memory_order_relaxed); }

  /// Current effective mode, \c phy_pipeline_mode::cpu until set() is called.
  static phy_pipeline_mode get() { return instance().load(std::memory_order_relaxed); }

private:
  static std::atomic<phy_pipeline_mode>& instance()
  {
    static std::atomic<phy_pipeline_mode> mode{phy_pipeline_mode::cpu};
    return mode;
  }

  static std::atomic<bool>& published()
  {
    static std::atomic<bool> flag{false};
    return flag;
  }
};

} // namespace ocudu
