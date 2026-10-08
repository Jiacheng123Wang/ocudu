// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "include/ocudu/support/ocudu_assert.h"
#include "include/ocudu/support/timers.h"
#include "metrics_producer.h"
#include "ocudu/support/executors/execute_until_success.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/support/synchronization/sync_event.h"
#include <chrono>
#include <string>
#include <vector>

namespace ocudu {
namespace app_services {

/// This controller class uses unique timer and triggers new metrics generation by the registered producers based on a
/// configured period.
class periodic_metrics_report_controller
{
public:
  /// Constructor receives timer object, report period and application metric configs.
  /// \param[in] producer_names_ Name of each producer in \c producers_, in the same order. Optional: an
  ///            empty vector makes the reports below name the producer by index.
  periodic_metrics_report_controller(std::vector<metrics_producer*> producers_,
                                     std::vector<std::string>       producer_names_,
                                     ocudulog::basic_logger&        logger_,
                                     timer_manager&                 timers_,
                                     task_executor&                 executor_,
                                     std::chrono::milliseconds      report_period_) :
    logger(logger_),
    executor(executor_),
    timers(timers_),
    timer(timers.create_unique_timer(executor)),
    report_period(report_period_),
    producers(std::move(producers_)),
    producer_names(std::move(producer_names_)),
    producer_max_us(producers.size(), 0),
    producer_warned(producers.size(), false)
  {
    ocudu_assert(timer.is_valid(), "Invalid timer passed to metrics controller");
    timer.set(report_period, [this]() { report_metrics(); });
  }

  /// Starts the metrics report timer.
  void start()
  {
    if (!report_period.count()) {
      return;
    }
    stop_manager.reset();

    sync_event wait_all;
    defer_until_success(executor, timers, [this, token = wait_all.get_token()]() mutable { timer.run(); });
    // Block waiting for the controller to start.
    wait_all.wait();
  }

  /// Stops the metrics report timer.
  void stop()
  {
    if (!report_period.count()) {
      return;
    }

    // Name the slow producers before waiting. The wait below cannot finish while a report_metrics() call
    // holds its token, and that call runs every producer in turn - so a producer that became slow is what
    // makes the whole gNB fail to stop, and the 5-second alarm then SIGKILLs it. A leg that ends that way
    // carries no clue about which producer it was; this line is that clue.
    report_slow_producers();

    // Signal stop to asynchronous timer thread.
    stop_manager.stop();
    // Stop the timer.
    timer.stop();
  }

private:
  /// Trigger metrics report in all registered producers.
  void report_metrics()
  {
    auto token = stop_manager.get_token();
    // Do not rearm the timer and process metrics if stop was requested.
    if (OCUDU_UNLIKELY(token.is_stop_requested())) {
      return;
    }

    // Rearm the timer.
    timer.run();

    // Command the producers to report their accumulated metrics, timed one by one: the shutdown waits for
    // this loop to finish (see stop()), so the cost of a single producer is a shutdown budget, not only a
    // reporting one.
    for (size_t i = 0; i != producers.size(); ++i) {
      const auto t0 = std::chrono::steady_clock::now();
      producers[i]->on_new_report_period();
      const auto elapsed_us =
          std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
      const auto elapsed = static_cast<uint64_t>(elapsed_us < 0 ? 0 : elapsed_us);
      if (elapsed > producer_max_us[i]) {
        producer_max_us[i] = elapsed;
      }
      if ((elapsed >= slow_producer_us) && !producer_warned[i]) {
        producer_warned[i] = true;
        logger.warning("metrics producer {} took {} ms in one report period (the timer runs every {} ms)",
                       producer_label(i),
                       elapsed / 1000,
                       report_period.count());
      }
    }
  }

  /// Name of producer \c i: its metric name when the caller supplied one, its index otherwise.
  std::string producer_label(size_t i) const
  {
    if (i < producer_names.size() && !producer_names[i].empty()) {
      return "'" + producer_names[i] + "'";
    }
    return "#" + std::to_string(i);
  }

  /// Reports every producer whose slowest period reached the threshold (see slow_producer_us).
  void report_slow_producers()
  {
    for (size_t i = 0; i != producers.size(); ++i) {
      if (producer_max_us[i] < slow_producer_us) {
        continue;
      }
      logger.warning("metrics producer {} took up to {} ms in one report period of {} ms",
                     producer_label(i),
                     producer_max_us[i] / 1000,
                     report_period.count());
    }
  }

  /// A producer that occupies this much of one report period is reported by name: it is both a reporting cost
  /// and, because the shutdown waits for the report in flight, a shutdown cost.
  static constexpr uint64_t slow_producer_us = 50'000;

  ocudulog::basic_logger& logger;
  task_executor&          executor;
  timer_manager&          timers;

  /// Timer object armed for configured report period.
  unique_timer timer;
  /// Metrics report period.
  std::chrono::milliseconds report_period{0};
  /// Manager used for stopping this controller.
  stop_event_source stop_manager;
  /// List of metrics producers managed by this controller.
  std::vector<metrics_producer*> producers;
  /// Name of each producer, in the same order (may be empty; see producer_label()).
  std::vector<std::string> producer_names;
  /// Longest a single period took per producer, and whether it was already reported.
  std::vector<uint64_t> producer_max_us;
  std::vector<bool>     producer_warned;
};

} // namespace app_services
} // namespace ocudu
