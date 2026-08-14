// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/support/io/io_broker.h"
#include "ocudu/support/synchronization/sync_event.h"

namespace ocudu {

class timer_manager;

/// \brief Interface for a timer source.
class io_timer_source
{
public:
  io_timer_source(timer_manager&            tick_sink_,
                  io_broker&                broker_,
                  task_executor&            executor,
                  std::chrono::milliseconds tick_period,
                  bool                      auto_start = true);
  ~io_timer_source();

  /// Resume ticking in case it was previously halted.
  void resume();

  /// \brief Request the timer source to stop ticking.
  /// Note: This call does not block, so a tick might take place after this call.
  void request_stop();

private:
  void create_subscriber(scoped_sync_token token);
  void destroy_subscriber(scoped_sync_token& token);

  void read_time(int raw_fd, scoped_sync_token& token);

#if defined(__APPLE__)
  // macOS has no timerfd. The timer is emulated with a non-blocking pipe: a dedicated thread waits on absolute
  // Mach time and writes one byte per tick to the write end. The read end is registered with the broker like any
  // other readable fd. See the implementation in io_timer_source.cpp.
  unique_fd         macos_create_timer_fd();
  void              macos_timer_tick_loop();
  void              macos_stop_timer_thread();

  unique_fd         timer_write_fd;
  unique_thread     timer_thread;
  std::atomic<bool> timer_stop{false};
#endif

  const std::chrono::milliseconds tick_period;
  timer_manager&                  tick_sink;
  io_broker&                      broker;
  task_executor&                  tick_exec;
  ocudulog::basic_logger&         logger;
  io_broker::subscriber           io_sub;

  // Current state of the timer source.
  std::atomic<bool> running{false};

  // Synchronization primitive to stop the timer source.
  // Note: shutdown_flag.reset() is only called during io_timer_source destruction. Do not confuse it with stop/resume
  // of the timer.
  sync_event shutdown_flag;
};

} // namespace ocudu
