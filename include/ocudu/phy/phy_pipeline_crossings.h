// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/phy/phy_pipeline_contract.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>

namespace ocudu {

/// \brief The host <-> device DATA crossings left in the uplink chain, counted where they happen.
///
/// ---- Why this exists ----
/// The `gpu` pipeline mode states its goal as a CROSSING COUNT, not as a latency. From
/// phy_pipeline_mode.h: the fused lane runs the whole IQ -> LLR chain "inside one device-side pipeline
/// with only two host <-> device *data* crossings (the IQ upload and the LLR download)".
///
/// Nothing measured that claim. The contract's other checks count SAMPLES ("host sample assembly"),
/// the GRID ("zero-copy wraps") and a compensation round trip ("cfo compensation"), and every one of
/// them passed on every air leg while the estimator was still reading three device scalars per hop and
/// handing the result back as kernel parameters - a dev -> host -> dev round trip that is neither of
/// the two crossings the mode allows. A mode whose stated property is not measured cannot be reached,
/// only asserted; and the property that was being asserted instead (sample copies) had already been
/// achieved and held for 24 consecutive legs.
///
/// ---- What counts ----
/// A crossing is the host TAKING device-produced data away, or handing device-DERIVED data back.
/// That is the leg which makes a device-side pipeline impossible, and it is what has to go to zero.
///
/// It does NOT count: submitting a command buffer, fencing, choosing a backend, or reading
/// configuration - control, not data. Neither does it count the IQ upload or the LLR download, which
/// the mode explicitly allows; those are the two the count is driven TO, not FROM.
///
/// \note Deliberately a plain counter and not an inferred quantity: the earlier attempts to attribute
/// the lane's gap by subtracting one measurement from another produced two wrong conclusions in a row
/// (see the design document). This one is incremented at the read, so it cannot be argued with.
class phy_pipeline_crossings
{
public:
  /// Counts one host read of data the device produced.
  static void count_host_read() { host_reads().fetch_add(1, std::memory_order_relaxed); }

  /// Counts one hop that consumed device output, so a total can be read as "per hop".
  static void count_device_hop() { device_hops().fetch_add(1, std::memory_order_relaxed); }

  static uint64_t get_host_reads() { return host_reads().load(std::memory_order_relaxed); }
  static uint64_t get_device_hops() { return device_hops().load(std::memory_order_relaxed); }

private:
  // Never destroyed: the report runs from an atexit handler, which runs after the static destructors
  // of this translation unit (the same reason ul_host_stats and the lane probe heap-allocate theirs).
  static std::atomic<uint64_t>& host_reads()
  {
    static std::atomic<uint64_t>* n = new std::atomic<uint64_t>(0);
    return *n;
  }
  static std::atomic<uint64_t>& device_hops()
  {
    static std::atomic<uint64_t>* n = new std::atomic<uint64_t>(0);
    return *n;
  }
};

/// \brief Registers the contract check that states the mode's crossing claim. Safe to call more than
/// once (a gNB builds one estimator per concurrent PUSCH thread).
///
/// The COUNT is printed in every published mode - it is the number this work has to drive to zero,
/// and hiding it in the modes that have not reached it yet would be exactly the mistake this check
/// exists to correct. The VERDICT belongs to `gpu` alone: cpu and cpu_gpu keep a crossing at every
/// module boundary by their own definition, so they claim nothing here and report "not applicable"
/// rather than passing or failing a requirement they never made.
inline void register_phy_pipeline_crossing_check()
{
  static std::once_flag once;
  std::call_once(once, []() {
    register_phy_pipeline_check(
        {"host device data crossings", []() -> std::optional<bool> {
           const uint64_t           reads = phy_pipeline_crossings::get_host_reads();
           const uint64_t           hops  = phy_pipeline_crossings::get_device_hops();
           const phy_pipeline_mode  mode  = phy_pipeline_mode_registry::get();
           std::fprintf(stderr,
                        "%llu host read(s) of device-produced data over %llu device hop(s) = %.2f per hop; "
                        "the fused lane (mode=gpu) allows 0 (its two crossings are the IQ upload and the "
                        "LLR download, which this does not count)",
                        static_cast<unsigned long long>(reads),
                        static_cast<unsigned long long>(hops),
                        (hops != 0) ? (static_cast<double>(reads) / static_cast<double>(hops)) : 0.0);
           if (!phy_pipeline_mode_registry::is_published() || (hops == 0)) {
             return std::optional<bool>{};
           }
           if (mode != phy_pipeline_mode::gpu) {
             return std::optional<bool>{};
           }
           return std::optional<bool>(reads == 0);
         }});
  });
}

} // namespace ocudu
