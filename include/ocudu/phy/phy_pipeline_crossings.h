// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/phy/phy_pipeline_contract.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

  /// Counts one host write of data the device will consume.
  ///
  /// \param[in] bytes How many bytes the host wrote, or 0 when the caller does not know. Only the
  ///            BYTE total is optional; the COUNT is what the verdict uses, because one host store
  ///            into a buffer the device reads is a crossing whether it moves one float or a whole
  ///            slot. The write side is the easy one to miss: the lane's buffers are zero-copy
  ///            mappings, so a host store into one is an ordinary assignment in the source and a
  ///            device-visible transfer at runtime, with no memcpy to find.
  static void count_host_write(uint64_t bytes = 0)
  {
    host_writes().fetch_add(1, std::memory_order_relaxed);
    if (bytes != 0) {
      host_write_bytes().fetch_add(bytes, std::memory_order_relaxed);
    }
  }

  /// \brief Declares that \p module has audited its host <-> device data touches and counts them here.
  ///
  /// The report lists the declarers NEXT TO the number, so the scope of a zero is visible with it.
  /// This is not decoration: the first version of this file counted the channel estimator's four read
  /// sites and nothing else, while the message said "the fused lane (mode=gpu) allows 0" - and that
  /// green OK was read (by its author) as "the whole lane is clean". It meant one module of four. A
  /// check that overstates its scope is worse than no check, because it ends the search.
  ///
  /// A module that has NOT audited its touches must not call this.
  static void declare_reporter(const char* module)
  {
    std::lock_guard<std::mutex> lock(reporters_mutex());
    for (std::size_t i = 0; i != nof_reporters(); ++i) {
      const char* r = reporters()[i];
      if ((r != nullptr) && (std::strcmp(r, module) == 0)) {
        return;
      }
    }
    if (nof_reporters() < kMaxReporters) {
      reporters()[nof_reporters()] = module;
    }
  }

  /// Prints the declarers, comma-separated, to \p out.
  ///
  /// Prints rather than returning a std::string on purpose: this header is included from translation
  /// units that sit INSIDE a namespace, where pulling in <string>/<vector>/<algorithm> makes libc++
  /// fail with errors like "no template named 'basic_ostream'". Keep this header to
  /// <atomic>/<cstdint>/<cstdio>/<cstring>/<mutex> and no more.
  static void print_reporters(std::FILE* out)
  {
    std::lock_guard<std::mutex> lock(reporters_mutex());
    if (nof_reporters() == 0) {
      std::fprintf(out, "<none>");
      return;
    }
    for (std::size_t i = 0; i != nof_reporters(); ++i) {
      std::fprintf(out, "%s%s", (i == 0) ? "" : ", ", reporters()[i]);
    }
  }

  /// Counts one hop that consumed device output, so a total can be read as "per hop".
  static void count_device_hop() { device_hops().fetch_add(1, std::memory_order_relaxed); }

  static uint64_t get_host_reads() { return host_reads().load(std::memory_order_relaxed); }
  static uint64_t get_host_writes() { return host_writes().load(std::memory_order_relaxed); }
  static uint64_t get_host_write_bytes() { return host_write_bytes().load(std::memory_order_relaxed); }
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
  static std::atomic<uint64_t>& host_writes()
  {
    static std::atomic<uint64_t>* n = new std::atomic<uint64_t>(0);
    return *n;
  }
  static std::atomic<uint64_t>& host_write_bytes()
  {
    static std::atomic<uint64_t>* n = new std::atomic<uint64_t>(0);
    return *n;
  }
  // Heap-allocated for the same reason as the counters (the report runs from an atexit handler), and
  // behind a mutex because modules declare from different threads.
  static constexpr std::size_t kMaxReporters = 8;
  static const char**          reporters()
  {
    static const char** v = new const char*[kMaxReporters]();
    return v;
  }
  /// Declares fill from 0 without holes, so scanning for the first null is exact and needs no second
  /// counter to keep in step.
  static std::size_t nof_reporters()
  {
    std::size_t n = 0;
    while ((n != kMaxReporters) && (reporters()[n] != nullptr)) {
      ++n;
    }
    return n;
  }
  static std::mutex& reporters_mutex()
  {
    static std::mutex* m = new std::mutex();
    return *m;
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
           const uint64_t          reads  = phy_pipeline_crossings::get_host_reads();
           const uint64_t          writes = phy_pipeline_crossings::get_host_writes();
           const uint64_t          bytes  = phy_pipeline_crossings::get_host_write_bytes();
           const uint64_t          hops   = phy_pipeline_crossings::get_device_hops();
           const phy_pipeline_mode mode   = phy_pipeline_mode_registry::get();
           const auto              per_hop = [hops](uint64_t n) {
             return (hops != 0) ? (static_cast<double>(n) / static_cast<double>(hops)) : 0.0;
           };
           std::fprintf(stderr,
                        "%llu host read(s) and %llu host write(s) (%llu bytes) of device data over %llu "
                        "device hop(s) = %.2f read(s) + %.2f write(s) per hop; the fused lane (mode=gpu) "
                        "allows 0 of each (its two crossings are the IQ upload and the LLR download, "
                        "which this counts neither of)",
                        static_cast<unsigned long long>(reads),
                        static_cast<unsigned long long>(writes),
                        static_cast<unsigned long long>(bytes),
                        static_cast<unsigned long long>(hops),
                        per_hop(reads),
                        per_hop(writes));
           // The SCOPE of the number, printed with the number. Whoever reads a 0 here has to be able
           // to see how much of the lane it covers - see declare_reporter().
           std::fprintf(stderr,
                        "  counted by the module(s) that audited their host <-> device data touches: ");
           phy_pipeline_crossings::print_reporters(stderr);
           std::fprintf(stderr, " (a module NOT listed here is not covered by this number)\n");
           if (!phy_pipeline_mode_registry::is_published() || (hops == 0)) {
             return std::optional<bool>{};
           }
           if (mode != phy_pipeline_mode::gpu) {
             return std::optional<bool>{};
           }
           return std::optional<bool>((reads == 0) && (writes == 0));
         }});
  });
}

} // namespace ocudu
