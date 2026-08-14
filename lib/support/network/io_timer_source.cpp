// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu/support/io/io_timer_source.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/support/error_handling.h"
#include "ocudu/support/io/io_broker.h"
#include "ocudu/support/timers.h"

#if !defined(__APPLE__)
#include <sys/timerfd.h>
#else
#include <fcntl.h>
#include <mach/mach_time.h>
#endif
#include <unistd.h>

using namespace ocudu;

#if !defined(__APPLE__)
static unique_fd create_timer_fd(std::chrono::milliseconds tick_period)
{
  using namespace std::chrono;

  auto timer_fd = unique_fd{::timerfd_create(CLOCK_MONOTONIC, 0)};
  report_fatal_error_if_not(timer_fd.is_open(), "Failed to create timer source (errno={})", ::strerror(errno));

  auto         tsecs     = duration_cast<seconds>(tick_period);
  auto         tnsecs    = duration_cast<nanoseconds>(tick_period) - duration_cast<nanoseconds>(tsecs);
  ::timespec   period    = {tsecs.count(), tnsecs.count()};
  ::itimerspec timerspec = {period, period};
  ::timerfd_settime(timer_fd.value(), 0, &timerspec, nullptr);

  return timer_fd;
}
#endif

io_timer_source::io_timer_source(timer_manager&            tick_sink_,
                                 io_broker&                broker_,
                                 task_executor&            executor,
                                 std::chrono::milliseconds tick_period_,
                                 bool                      auto_start) :
  tick_period(tick_period_),
  tick_sink(tick_sink_),
  broker(broker_),
  tick_exec(executor),
  logger(ocudulog::fetch_basic_logger("IO-EPOLL"))
{
  if (auto_start) {
    running.store(true, std::memory_order_relaxed);
    create_subscriber(shutdown_flag.get_token());
  }
}

io_timer_source::~io_timer_source()
{
  request_stop();
  // The dtor of stop_flag will block until all tasks using the token have completed.
}

void io_timer_source::resume()
{
  auto prev = running.exchange(true, std::memory_order_acq_rel);
  if (prev) {
    // Already started. No need to dispatch task. Early exit.
    return;
  }

  // Dispatch task to start ticking.
  // Note: Token is used to protect the asynchronous callback from outliving this object.
  while (not tick_exec.defer(
      [this, token = shutdown_flag.get_token()]() mutable { create_subscriber(std::move(token)); })) {
    // We cannot allow the command to be lost. Retry until we succeed.
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
}

void io_timer_source::request_stop()
{
  // Request in a non-blocking fashion, the stop of the timer.
  running.store(false, std::memory_order_release);
}

void io_timer_source::create_subscriber(scoped_sync_token token)
{
  // Note: Called inside the ticking executor, except for in the ctor.

  if (io_sub.registered()) {
    // Already created.
    return;
  }

  if (not running.load(std::memory_order_acquire)) {
    // The state has changed in the meantime and the subscriber wasn't created.
    return;
  }

  logger.info("Starting IO timer ticking source...");
#if defined(__APPLE__)
  auto fd = macos_create_timer_fd();
#else
  auto fd = create_timer_fd(tick_period);
#endif

  const int raw_fd = fd.value();
  io_sub = broker.register_fd(std::move(fd), tick_exec, [this, raw_fd, token]() mutable { read_time(raw_fd, token); });
  report_fatal_error_if_not(io_sub.registered(), "Failed to create timer source");
}

void io_timer_source::destroy_subscriber(scoped_sync_token& token)
{
  if (not io_sub.registered()) {
    // Already destroyed.
    return;
  }
  logger.info("Stopping IO timer ticking source...");
#if defined(__APPLE__)
  // Stop the tick thread and close the write end before the broker closes the read end, avoiding SIGPIPE on the
  // tick thread.
  macos_stop_timer_thread();
#endif
  // Unregister the fd from the broker.
  // Note: Destroying the subscriber will destroy the token saved in its callback capture in the io_broker backend.
  // We want to destroy the token here though to have deterministic time of destruction, so we move the token out first.
  auto token_tmp = std::move(token);
  io_sub.reset();
  logger.info("IO timer source stopped.");
  token_tmp.reset();
}

void io_timer_source::read_time(int raw_fd, scoped_sync_token& token)
{
  // Note: Called inside the ticking executor.

  if (not running.load(std::memory_order_acquire)) {
    // Destroy subscriber and signal the completion of the stop by resetting the token.
    destroy_subscriber(token);
    // Note: Do not touch any variable here as the ~io_timer_source() might be running concurrently.
    return;
  }

  uint64_t nof_expirations = 0;
#if !defined(__APPLE__)
  int n = ::read(raw_fd, &nof_expirations, sizeof(nof_expirations));
  if (n < 0) {
    logger.error("Failed to read timerfd (errno={})", ::strerror(errno));
    return;
  }
  if (n == 0) {
    logger.warning("Timerfd read returned 0");
    return;
  }
#else
  // Drain the timer pipe: every byte represents one tick, including ticks accumulated while the executor was busy.
  uint8_t tick_buf[64];
  while (true) {
    ssize_t n = ::read(raw_fd, tick_buf, sizeof(tick_buf));
    if (n > 0) {
      nof_expirations += static_cast<uint64_t>(n);
      continue;
    }
    if (n < 0 and errno == EINTR) {
      continue;
    }
    if (n == 0) {
      // EOF: the write end of the pipe was closed.
      logger.warning("Timer pipe read end reached EOF");
    } else if (errno != EAGAIN) {
      logger.error("Failed to read timer pipe (errno={})", ::strerror(errno));
    }
    break;
  }
#endif

  while (nof_expirations-- > 0) {
    // Tick timers.
    tick_sink.tick();
  }
}

#if defined(__APPLE__)
unique_fd io_timer_source::macos_create_timer_fd()
{
  int pipe_fds[2];
  if (::pipe(pipe_fds) != 0) {
    report_fatal_error("Failed to create timer pipe (errno={})", ::strerror(errno));
  }

  // Make both ends non-blocking: the read end so read_time() can drain it until EAGAIN, and the write end so the
  // tick thread never blocks when the consumer is stalled.
  for (int fd : {pipe_fds[0], pipe_fds[1]}) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags == -1 or ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
      report_fatal_error("Failed to set timer pipe to non-blocking (errno={})", ::strerror(errno));
    }
  }

  timer_write_fd = unique_fd{pipe_fds[1]};
  timer_stop.store(false, std::memory_order_release);
  timer_thread   = unique_thread("io_timer_tick", os_thread_realtime_priority::no_realtime(), [this]() {
    macos_timer_tick_loop();
  });

  // Return the read end to be registered with the io_broker.
  return unique_fd{pipe_fds[0]};
}

void io_timer_source::macos_timer_tick_loop()
{
  // Convert the tick period from nanoseconds to Mach absolute time units using the Mach timebase info.
  // Mach time is monotonic and unaffected by wall-clock changes.
  mach_timebase_info_data_t timebase_info;
  ::mach_timebase_info(&timebase_info);
  const uint64_t period_ticks =
      static_cast<uint64_t>(tick_period.count()) * 1'000'000ull * timebase_info.denom / timebase_info.numer;

  // Wait on absolute deadlines: unlike relative sleeps, this does not accumulate drift.
  uint64_t deadline = ::mach_absolute_time() + period_ticks;
  while (not timer_stop.load(std::memory_order_acquire)) {
    ::mach_wait_until(deadline);
    deadline += period_ticks;

    if (timer_stop.load(std::memory_order_acquire)) {
      break;
    }

    // Signal one tick.
    uint8_t  dummy = 1;
    ssize_t  ret   = ::write(timer_write_fd.value(), &dummy, sizeof(dummy));
    if (ret < 0 and errno == EAGAIN) {
      // The pipe is full: the consumer is far behind (the pipe buffers about 8 s of ticks at 1 ms). Drop the
      // accumulated backlog and resync the deadline to now to avoid a tight spin through the missed deadlines.
      deadline = ::mach_absolute_time() + period_ticks;
    } else if (ret < 0 and errno != EINTR) {
      // The read end was closed (e.g., the broker deregistered the pipe): nothing to signal anymore.
      break;
    }
  }
}

void io_timer_source::macos_stop_timer_thread()
{
  timer_stop.store(true, std::memory_order_release);
  if (timer_thread.running()) {
    // The tick thread checks the flag after each mach_wait_until(), so the join blocks for at most one tick period.
    timer_thread.join();
  }
  timer_stop.store(false, std::memory_order_release);
  if (timer_write_fd.is_open() and not timer_write_fd.close()) {
    logger.error("Failed to close timer pipe write end: {}", ::strerror(errno));
  }
}
#endif
