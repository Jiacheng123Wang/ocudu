// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
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
///
/// A second series measures the pure LDPC decoder latency (first codeblock decode invocation -> the same CRC-OK
/// completion as above): record_ldpc_start() is called by the PUSCH codeblock task right before the LDPC decoder is
/// invoked. The start is a single last-write-wins timestamp: with the light traffic of a single UE there is one TB
/// in flight at a time, so a start left behind by a CRC-failed TB is overwritten by the next TB's decode before its
/// completion. A staleness guard (2 ms) drops leftover starts that predate the current TB (e.g. a retransmission
/// that needs no decode), so stale starts are never paired with a foreign completion.
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

  /// Records the start of the LDPC decoder (call right before the first codeblock decode of a transport block).
  void record_ldpc_start()
  {
    std::lock_guard<std::mutex> lock(mutex);
    pending_ldpc_start = std::chrono::high_resolution_clock::now();
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
    if (it != pending_starts.end()) {
      double latency_us =
          static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(now - it->second).count());
      pending_starts.erase(it);
      latencies_us.push_back(latency_us);
    }
    // LDPC decoder latency: the pending start belongs to this TB only if it is fresh (a decode-to-completion
    // cannot exceed the ~300 us pipeline, so 2 ms also covers multi-CB TBs; stale starts are leftovers of
    // CRC-failed TBs or retransmissions that needed no decode and are dropped).
    if (pending_ldpc_start.has_value()) {
      const auto ldpc_us =
          std::chrono::duration_cast<std::chrono::microseconds>(now - *pending_ldpc_start);
      pending_ldpc_start.reset();
      if (ldpc_us.count() <= 2000) {
        ldpc_latencies_us.push_back(static_cast<double>(ldpc_us.count()));
      }
    }
  }

  /// Prints the statistics of the recorded latencies. Called once during the application shutdown.
  void report()
  {
    std::vector<double> sorted_pipeline;
    std::vector<double> sorted_ldpc;
    {
      std::lock_guard<std::mutex> lock(mutex);
      sorted_pipeline = latencies_us;
      sorted_ldpc      = ldpc_latencies_us;
    }
    if (sorted_pipeline.empty()) {
      std::fprintf(stderr, "[ul_pipeline] no CRC-OK samples recorded\n");
      return;
    }
    std::sort(sorted_pipeline.begin(), sorted_pipeline.end());
    double sum = 0;
    for (double v : sorted_pipeline) {
      sum += v;
    }
    auto pct = [](const std::vector<double>& sorted, double p) {
      return sorted[static_cast<size_t>((sorted.size() - 1) * p)];
    };
    // Report to stderr (guaranteed to be visible at the shutdown, unlike the logging backend) and to the logs.
    std::fprintf(stderr,
                 "[ul_pipeline] samples=%zu mean=%.1fus median=%.1fus min=%.1fus max=%.1fus p95=%.1fus p99=%.1fus\n",
                 sorted_pipeline.size(),
                 sum / static_cast<double>(sorted_pipeline.size()),
                 pct(sorted_pipeline, 0.5),
                 sorted_pipeline.front(),
                 sorted_pipeline.back(),
                 pct(sorted_pipeline, 0.95),
                 pct(sorted_pipeline, 0.99));
    if (sorted_ldpc.empty()) {
      std::fprintf(stderr, "[ul_ldpc_decode] no samples recorded\n");
      return;
    }
    std::sort(sorted_ldpc.begin(), sorted_ldpc.end());
    sum = 0;
    for (double v : sorted_ldpc) {
      sum += v;
    }
    std::fprintf(stderr,
                 "[ul_ldpc_decode] samples=%zu mean=%.1fus median=%.1fus min=%.1fus max=%.1fus p95=%.1fus p99=%.1fus\n",
                 sorted_ldpc.size(),
                 sum / static_cast<double>(sorted_ldpc.size()),
                 pct(sorted_ldpc, 0.5),
                 sorted_ldpc.front(),
                 sorted_ldpc.back(),
                 pct(sorted_ldpc, 0.95),
                 pct(sorted_ldpc, 0.99));
  }

private:
  ul_pipeline_probe() = default;

  std::mutex mutex;
  std::map<uint64_t, std::chrono::time_point<std::chrono::high_resolution_clock>> pending_starts;
  std::vector<double> latencies_us;
  /// Last-write-wins timestamp of the current TB's LDPC decoder start (see record_ldpc_start()).
  std::optional<std::chrono::time_point<std::chrono::high_resolution_clock>> pending_ldpc_start;
  std::vector<double> ldpc_latencies_us;
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
  void record_ldpc_start() {}
  void record_end_crc_ok(uint64_t /*slot*/) {}
  void report() {}

private:
  ul_pipeline_probe() = default;
};

#endif

} // namespace ocudu
