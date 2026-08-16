// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
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

  /// Records the start of the UL processing for the current slot (call from the lower PHY baseband processor).
  void record_start()
  {
    std::lock_guard<std::mutex> lock(mutex);
    pending_starts.emplace_back(std::chrono::high_resolution_clock::now());
    // Bound the queue: an entry older than this is unmatched (e.g., idle slots), drop it to keep the FIFO aligned.
    if (pending_starts.size() > 64) {
      pending_starts.pop_front();
    }
  }

  /// Records the completion of the UL processing of a transport block whose CRC check passed.
  void record_end_crc_ok()
  {
    std::chrono::time_point<std::chrono::high_resolution_clock> now = std::chrono::high_resolution_clock::now();

    std::lock_guard<std::mutex> lock(mutex);
    if (pending_starts.empty()) {
      return;
    }
    double latency_us = static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(now - pending_starts.front()).count());
    pending_starts.pop_front();
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
      ocudulog::fetch_basic_logger("ALL").info("[ul_pipeline] no CRC-OK samples recorded");
      return;
    }
    std::sort(sorted.begin(), sorted.end());
    double sum = 0;
    for (double v : sorted) {
      sum += v;
    }
    auto pct = [&sorted](double p) { return sorted[static_cast<size_t>((sorted.size() - 1) * p)]; };
    ocudulog::fetch_basic_logger("ALL").info(
        "[ul_pipeline] samples={} mean={:.1f}us median={:.1f}us min={:.1f}us max={:.1f}us p95={:.1f}us p99={:.1f}us",
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
  std::deque<std::chrono::time_point<std::chrono::high_resolution_clock>> pending_starts;
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
  void record_start() {}
  void record_end_crc_ok() {}
  void report() {}

private:
  ul_pipeline_probe() = default;
};

#endif

} // namespace ocudu
