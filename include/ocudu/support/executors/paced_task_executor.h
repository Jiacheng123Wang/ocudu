// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/adt/mpmc_queue.h"
#include "ocudu/support/executors/task_executor.h"
#include "ocudu/support/executors/unique_thread.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

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
  /// \param nof_threads_    How many threads share the grid and this queue. ONE thread serialises whatever the
  ///                        lane used to run concurrently, and the first air sweep measured exactly that: the
  ///                        paced arm sat at ~3.2 Mbit/s against the pool's 6.74 whatever the band, because a
  ///                        hop's chain costs more WALL time (GPU waits included) than a slot, so one thread
  ///                        cannot sustain the slot rate the pool's two concurrent hops could. N threads keep the
  ///                        grid and restore the concurrency.
  /// \param prio, mask      Passed to the threads the same way every other worker gets them.
  ///
  /// \note THE BAND IS CLAMPED TO LESS THAN THE PERIOD, and the clamp is a structural rule rather than a
  ///       preference: a wait longer than one slot spans several ticks, so the thread can no longer keep one
  ///       tick per slot at all - measured on the sweep, a 4 ms band left the loop ticking 257 times a second
  ///       against the 2000 it is supposed to, and BOTH the goodput and the tail got worse than at 500 us.
  ///       A caller that asks for more gets the cap, and the report says so.
  paced_task_executor(std::string                      thread_name,
                      unsigned                         queue_size,
                      std::chrono::nanoseconds         slot_duration,
                      std::chrono::nanoseconds         lead,
                      std::chrono::nanoseconds         max_wait,
                      unsigned                         nof_threads_ = 1,
                      os_thread_realtime_priority      prio = os_thread_realtime_priority::no_realtime(),
                      const os_sched_affinity_bitmask& mask = {});

  ~paced_task_executor() override;

  /// The band the loop actually uses. Exposed because the clamp is a rule a leg must be able to SEE: a caller
  /// that asked for 4 ms gets 250 us at 30 kHz, and a report that silently used the smaller number would make
  /// the sweep's own table unreadable.
  [[nodiscard]] std::chrono::nanoseconds get_max_wait() const { return max_wait; }

  [[nodiscard]] bool execute(unique_task task) override;
  [[nodiscard]] bool defer(unique_task task) override;

  /// \brief Submits \p task to the grid and returns once it HAS RUN - the hand-off a caller uses when the
  /// ORDER of the operation matters and it cannot be fire-and-forget (plan doc §11.26: the lane's commit).
  ///
  /// WHY A SECOND ENTRY POINT. `execute()` is right for work whose result is awaited somewhere else (the lane's
  /// hop: the caller returns and a later stage waits). It is wrong for an operation the caller must observe as
  /// done before it continues - and the commit is exactly that, because the very next thing the committing
  /// thread does is `wait_committed()`, which waits on a command buffer that must have been committed first.
  ///
  /// \param timeout When the task has not run within this budget the call gives up, returns false, and THE
  ///        TASK IS NOT RUN - the caller must then do the work itself. That fallback is deliberate: a hand-off
  ///        that can silently lose an operation is worse than no hand-off, and a shutting-down executor, a
  ///        saturated queue and a stopped thread all have to land somewhere safe.
  [[nodiscard]] bool execute_and_wait(unique_task task, std::chrono::nanoseconds timeout);

  /// The accounting a leg reads: how many ticks ran work, how many were skipped, and how late the loop was.
  ///
  /// \note With N threads the counters are the SUM over them, so `ticks` is N times the number of grid ticks
  ///       the executor lived through - each thread keeps its own tick and they share the grid. The report
  ///       prints `threads=`, which is what turns the sum back into a per-thread rate.
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

  /// Prints this executor's accounting, as it stands. Called by the destructor (below) and by a leg that wants
  /// the reading while the run is still going.
  void report() const;

  /// \brief Prints EVERY LIVE paced executor, and this - not a pointer to one - is what an exit path registers.
  ///
  /// WHY, and it is the p233/p234/p235 abort: the exit paths used to call a `static paced_task_executor*` the
  /// mapper held, and on an air leg that object is a local of `main` - it is DESTROYED before the atexit
  /// handlers run. The handler then locked a mutex inside freed memory, `pthread_mutex_lock` said EINVAL, the
  /// std::system_error escaped and the process died with `Abort trap: 6` **partway through the exit report**,
  /// taking the very readings the report existed for with it (the three legs have no [ul_pipeline] contract
  /// lines and no [paced_exec] account at all). The loopback had "verified" this path and could not: freed
  /// memory that has not been reused still holds a working mutex, so the false pass was reading freed bytes.
  ///
  /// The registry is the fix rather than a flag: an executor adds itself in its constructor and REMOVES itself
  /// in its destructor, so a report can only ever see objects that are alive - and when they are all gone it
  /// prints nothing at all, which is the honest answer.
  static void report_all_live();

  /// How many executors are in that registry right now. Exposed because the registry's whole job is to be asked
  /// this: a leg (and this file's own test) must be able to tell "nothing to report" from "reported the wrong
  /// object", and the abort this replaced was exactly the difference between the two.
  static size_t nof_live();

private:
  void run();

  using queue_t = concurrent_queue<unique_task, concurrent_queue_policy::lockfree_mpmc, concurrent_queue_wait_policy::sleep>;

  std::string       name;
  std::chrono::nanoseconds slot_duration;
  std::chrono::nanoseconds lead;
  std::chrono::nanoseconds max_wait;            ///< effective, i.e. after the clamp below the period
  std::chrono::nanoseconds max_wait_requested;  ///< what the caller asked for, so the report can show the cap
  unsigned                 nof_threads = 1;
  queue_t                       pending;
  std::vector<unique_thread>    threads;
  std::atomic<bool> running{true};

  mutable std::mutex stat_mutex;
  stats              counters;
};

} // namespace ocudu
