// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/adt/mpmc_queue.h"
#include "ocudu/support/executors/task_executor.h"
#include "ocudu/support/executors/unique_thread.h"

#include <atomic>
#include <cstdint>

namespace ocudu {

/// \brief A single-thread executor that runs its tasks ON A GRID instead of as they arrive (plan doc §11.17).
///
/// WHY, AND WHAT IT IS NOT. Every lever this line has measured says the same thing from a different side:
/// the UL pipeline's commit instants are today a function of WHEN THE DATA ARRIVED, so transport jitter and
/// scheduling stalls move them, and the `max` that matters is the cadence, not the duration. Delaying the work
/// until a grid instant does not fix that - it was tried (p224/p225) and it PAYS the lead as latency while the
/// grid follows the arrivals. What fixes it is the thread owning its own clock and being willing to lose a
/// slot: it wakes at the grid, waits a BOUNDED time for work, runs what arrived, and if nothing arrived it
/// counts a skip and goes back to sleep. The cadence is then the thread's, not the data's.
///
/// THE ELASTIC BAND, which is the user's design (2026-10-03):
///
///     wake_at = grid_tick + lead                  the earliest the thread may act
///     [wake_at, wake_at + max_wait]               poll for a task
///         a task is there (or arrives)  -> run it; the commit lands somewhere in the band
///         nothing by the end of the band -> SKIP the tick and count it
///
/// `max_wait = 0` is fully rigid (a task that is not already queued misses its tick); a very large `max_wait`
/// falls back to "wait indefinitely", which loses no slot and is exactly the behaviour the legs already
/// measured as the one that cannot be made deterministic. The skip rate is therefore an OUTPUT of the choice,
/// not a target - what decides `max_wait` is the application (ping, iperf3), per the same user ruling.
///
/// SCOPE. macOS-only pacing on top of a platform-neutral executor: with the knob off - the default, and the
/// only behaviour Linux can reach - `compat::lane_grid_enabled()` is false, the loop runs the classic
/// "pop and run" as fast as tasks arrive, and nothing about this file changes a leg. The THREAD ITSELF still
/// exists in that case, which is deliberate: the leg then measures the executor's cost separately from the
/// pacing.
class paced_task_executor : public task_executor
{
public:
  /// \param thread_name     Name of the thread it creates. The Mach time constraint is declared by name
  ///                        (OCUDU_SCHED_TIME_CONSTRAINT), so this is the handle the declaration uses.
  /// \param queue_size      Pending tasks the queue can hold.
  /// \param slot_duration   The grid's period: one slot (see plan doc §11.4; parameterised by SCS, not fixed).
  /// \param lead            How long after the grid tick the loop starts looking for work.
  /// \param max_wait        How long past `lead` it keeps looking before it skips the tick.
  /// \param prio, mask      Passed to the thread the same way every other worker gets them.
  paced_task_executor(std::string                      thread_name,
                      unsigned                         queue_size,
                      std::chrono::nanoseconds         slot_duration,
                      std::chrono::nanoseconds         lead,
                      std::chrono::nanoseconds         max_wait,
                      os_thread_realtime_priority      prio = os_thread_realtime_priority::no_realtime(),
                      const os_sched_affinity_bitmask& mask = {});

  ~paced_task_executor() override;

  [[nodiscard]] bool execute(unique_task task) override;
  [[nodiscard]] bool defer(unique_task task) override;

  /// The accounting a leg reads: how many ticks ran work, how many were skipped, and how late the loop was.
  struct stats {
    uint64_t ticks    = 0;
    uint64_t ran      = 0; ///< ticks that ran at least one task
    uint64_t skipped  = 0; ///< ticks where nothing arrived inside the band
    uint64_t tasks    = 0;
    /// How far past `wake_at` the first task of a tick was actually run: the band's own precision, the same
    /// quantity the lane grid reports as `overshoot` for its clamp.
    int64_t late_sum_us = 0;
    int64_t late_max_us = 0;
  };
  stats get_stats() const;

  /// Prints one line per paced executor, next to the other reports. Registered by the caller, not by atexit,
  /// for the reason the receive pool's report spells out: the gNB's stop path dumps the P0 readings and then
  /// raises SIGKILL.
  void report() const;

private:
  void run();

  using queue_t = concurrent_queue<unique_task, concurrent_queue_policy::lockfree_mpmc, concurrent_queue_wait_policy::sleep>;

  std::string       name;
  std::chrono::nanoseconds slot_duration;
  std::chrono::nanoseconds lead;
  std::chrono::nanoseconds max_wait;
  queue_t           pending;
  unique_thread     thread;
  std::atomic<bool> running{true};

  mutable std::mutex stat_mutex;
  stats              counters;
};

} // namespace ocudu
