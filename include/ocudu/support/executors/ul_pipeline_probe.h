// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

#if defined(OCUDU_FLOW_PROBES)
#include "ocudu/ocudulog/ocudulog.h"
#endif

namespace ocudu {

#if defined(OCUDU_FLOW_PROBES)

/// \brief Measures the UL compute pipeline latency (IQ samples received -> LDPC decoded with CRC OK).
///
/// - record_start() is called by the lower PHY baseband processor right after the IQ samples of a slot are received.
/// - record_end_crc_ok() is called by the PUSCH decoder notifier when the transport block CRC check passes.
///
/// Starts and ends are matched in order (FIFO): with the light traffic of a single UE there is one PUSCH in flight
/// at a time, so the first uncompleted start corresponds to the next CRC-OK completion. Latencies are accumulated in
/// memory; report() prints the statistics through the logging system (call it during the application shutdown).
class ul_pipeline_probe
{
public:
  static ul_pipeline_probe& get()
  {
    static ul_pipeline_probe instance;
    return instance;
  }

  /// Records the start of the UL processing of a slot (call from the lower PHY baseband processor).
  /// \param[in] slot Slot number (SFN-referenced slot count, matching the FAPI slot indications).
  void record_start(uint64_t slot)
  {
    std::lock_guard<std::mutex> lock(mutex);
    pending_starts[slot] = std::chrono::high_resolution_clock::now();
    // Bound the registry: unmatched entries belong to idle slots (no PUSCH), drop the oldest.
    if (pending_starts.size() > 256) {
      pending_starts.erase(pending_starts.begin());
    }
  }

  /// Records the completion of the UL processing of a transport block whose CRC check passed.
  /// \param[in] slot Slot number of the PUSCH (same reference as record_start; a small offset is tolerated).
  void record_end_crc_ok(uint64_t slot)
  {
    std::chrono::time_point<std::chrono::high_resolution_clock> now = std::chrono::high_resolution_clock::now();

    std::lock_guard<std::mutex> lock(mutex);
    auto it = pending_starts.find(slot);
    if (it == pending_starts.end() && slot > 0) {
      it = pending_starts.find(slot - 1);
    }
    if (it == pending_starts.end() && slot > 1) {
      it = pending_starts.find(slot - 2);
    }
    if (it == pending_starts.end()) {
      return;
    }
    double latency_us =
        static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(now - it->second).count());
    pending_starts.erase(it);
    latencies_us.push_back(latency_us);
  }

  /// Prints the statistics of the recorded latencies. Called once during the application shutdown.
  void report()
  {
    std::vector<double> sorted;
    {
      std::lock_guard<std::mutex> lock(mutex);
      sorted = latencies_us;
    }
    if (sorted.empty()) {
      std::fprintf(stderr, "[ul_pipeline] no CRC-OK samples recorded\n");
      return;
    }
    std::sort(sorted.begin(), sorted.end());
    double sum = 0;
    for (double v : sorted) {
      sum += v;
    }
    auto pct = [&sorted](double p) { return sorted[static_cast<size_t>((sorted.size() - 1) * p)]; };
    // Report to stderr (guaranteed to be visible at the shutdown, unlike the logging backend) and to the logs.
    std::fprintf(stderr,
                 "[ul_pipeline] samples=%zu mean=%.1fus median=%.1fus min=%.1fus max=%.1fus p95=%.1fus p99=%.1fus\n",
                 sorted.size(),
                 sum / static_cast<double>(sorted.size()),
                 pct(0.5),
                 sorted.front(),
                 sorted.back(),
                 pct(0.95),
                 pct(0.99));
  }

private:
  ul_pipeline_probe() = default;

  std::mutex mutex;
  std::map<uint64_t, std::chrono::time_point<std::chrono::high_resolution_clock>> pending_starts;
  std::vector<double> latencies_us;
};

#else // not OCUDU_FLOW_PROBES: no-op implementation with zero overhead.

class ul_pipeline_probe
{
public:
  static ul_pipeline_probe& get()
  {
    static ul_pipeline_probe instance;
    return instance;
  }
  void record_start(uint64_t /*slot*/) {}
  void record_end_crc_ok(uint64_t /*slot*/) {}
  void report() {}

private:
  ul_pipeline_probe() = default;
};

#endif

} // namespace ocudu
