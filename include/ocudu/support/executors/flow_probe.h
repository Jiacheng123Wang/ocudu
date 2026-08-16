// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include <chrono>
#include <cstdio>
#include <optional>
#include <string>

#if defined(OCUDU_FLOW_PROBES)
#include "ocudu/ocudulog/ocudulog.h"
#endif

namespace ocudu {

#if defined(OCUDU_FLOW_PROBES)

/// \brief One-line-per-second data-path probe used to compare the transport behavior of the gNB across platforms
/// (pull rate, reply sizes, production rate). Reports through the asynchronous logging system at debug level.
///
/// Thread-unsafe by design: each instance must be used from a single thread (the channel async executor thread or
/// the baseband executor thread).
class flow_probe
{
public:
  explicit flow_probe(std::string label_) : label(std::move(label_)) {}

  /// Registers one event, optionally carrying a sample count and a latency observation (microseconds).
  void event(uint64_t n_samples = 0, uint64_t wait_us = 0)
  {
    ++events;
    samples += n_samples;
    total_wait_us += wait_us;
  }

  /// Called periodically (e.g., every loop iteration): reports and resets the per-second counters once per second.
  void tick()
  {
    auto now = std::chrono::steady_clock::now();
    if (now < next_report) {
      return;
    }

    static auto& logger = ocudulog::fetch_basic_logger("ALL");

    double dt = std::chrono::duration<double>(now - last_report).count();
    if (dt > 0) {
      logger.debug("[flow_probe:{}] {:.1f} ev/s, {:.0f} samples/s, avg {:.0f} samples/ev, avg wait {:.0f} us, totals {} ev / {} samples",
                   label,
                   static_cast<double>(events) / dt,
                   static_cast<double>(samples) / dt,
                   events != 0 ? static_cast<double>(samples) / static_cast<double>(events) : 0.0,
                   events != 0 ? static_cast<double>(total_wait_us) / static_cast<double>(events) : 0.0,
                   total_events + events,
                   total_samples + samples);
    }

    total_events += events;
    total_samples += samples;
    events        = 0;
    samples       = 0;
    total_wait_us = 0;
    last_report   = now;
    next_report   = now + std::chrono::seconds(1);
  }

private:
  std::string                           label;
  uint64_t                              events        = 0;
  uint64_t                              samples       = 0;
  uint64_t                              total_wait_us = 0;
  uint64_t                              total_events  = 0;
  uint64_t                              total_samples = 0;
  std::chrono::steady_clock::time_point last_report   = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point next_report = std::chrono::steady_clock::now() + std::chrono::seconds(1);
};

/// \brief Per-event inter-arrival gap statistics (min/avg/max per second), for timing jitter measurement.
///
/// Thread-unsafe by design: each instance must be used from a single thread.
class flow_interval_probe
{
public:
  explicit flow_interval_probe(std::string label_) : label(std::move(label_)) {}

  /// Registers one event; the gap to the previous event is accumulated.
  void event()
  {
    auto now = std::chrono::steady_clock::now();
    if (last_event.has_value()) {
      double gap_us =
          static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(now - *last_event).count());
      ++events;
      sum_gap_us += gap_us;
      if (gap_us < min_gap_us or min_gap_us < 0) {
        min_gap_us = gap_us;
      }
      if (gap_us > max_gap_us) {
        max_gap_us = gap_us;
      }
    }
    last_event = now;
  }

  /// Called periodically: reports and resets the per-second statistics once per second.
  void tick()
  {
    auto now = std::chrono::steady_clock::now();
    if (now < next_report) {
      return;
    }

    static auto& logger = ocudulog::fetch_basic_logger("ALL");

    if (events > 0) {
      logger.debug("[flow_jitter:{}] {} ev, gap min {:.0f} us, avg {:.0f} us, max {:.0f} us",
                   label,
                   events,
                   min_gap_us,
                   sum_gap_us / static_cast<double>(events),
                   max_gap_us);
    }

    events      = 0;
    sum_gap_us  = 0;
    min_gap_us  = -1;
    max_gap_us  = 0;
    last_report = now;
    next_report = now + std::chrono::seconds(1);
  }

private:
  std::string                                        label;
  std::optional<std::chrono::steady_clock::time_point> last_event;
  uint64_t                                           events     = 0;
  double                                             sum_gap_us = 0;
  double                                             min_gap_us = -1;
  double                                             max_gap_us = 0;
  std::chrono::steady_clock::time_point              last_report = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point next_report = std::chrono::steady_clock::now() + std::chrono::seconds(1);
};

#else // not OCUDU_FLOW_PROBES: no-op implementations with zero overhead.

class flow_probe
{
public:
  explicit flow_probe(std::string /*label_*/) {}
  void event(uint64_t /*n_samples*/ = 0, uint64_t /*wait_us*/ = 0) {}
  void tick() {}
};

class flow_interval_probe
{
public:
  explicit flow_interval_probe(std::string /*label_*/) {}
  void event() {}
  void tick() {}
};

#endif

} // namespace ocudu
