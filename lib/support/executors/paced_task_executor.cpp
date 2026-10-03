// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu/support/executors/paced_task_executor.h"
#include "ocudu/support/macos_compat.h"
#include "ocudu/support/scheduling/thread_sched_snapshot.h"

#include <chrono>
#include <cstdio>

using namespace ocudu;

namespace {

int64_t steady_now_ns()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

} // namespace

paced_task_executor::paced_task_executor(std::string                      thread_name,
                                         unsigned                         queue_size,
                                         std::chrono::nanoseconds         slot_duration_,
                                         std::chrono::nanoseconds         lead_,
                                         std::chrono::nanoseconds         max_wait_,
                                         os_thread_realtime_priority      prio,
                                         const os_sched_affinity_bitmask& mask) :
  name(std::move(thread_name)),
  slot_duration(slot_duration_),
  lead(lead_),
  max_wait(max_wait_),
  pending(queue_size, std::chrono::microseconds{50}),
  thread(name, prio, mask, [this]() { run(); })
{
}

paced_task_executor::~paced_task_executor()
{
  // THE FLAG ALONE DOES NOT STOP IT, and a test hung for ten minutes to say so: with the pacing off the loop is
  // parked inside `pop_blocking`, which `running` cannot interrupt. A no-op task is pushed after the flag to
  // wake it wherever it is - parked on the queue, or sleeping between two grid ticks - and the join is then
  // bounded by one queue wake-up. (The queued tasks behind it are NOT drained: a leg's stop must not run a
  // backlog it was told to abandon.)
  running.store(false, std::memory_order_relaxed);
  (void)pending.try_push(unique_task([]() {}));
  thread.join();
}

bool paced_task_executor::execute(unique_task task)
{
  return pending.try_push(std::move(task));
}

bool paced_task_executor::defer(unique_task task)
{
  return pending.try_push(std::move(task));
}

paced_task_executor::stats paced_task_executor::get_stats() const
{
  std::lock_guard<std::mutex> lock(stat_mutex);
  return counters;
}

void paced_task_executor::run()
{
  auto consumer = pending.create_consumer();

  // The pacing is macOS-only and off by default, so a Linux leg - and a macOS leg without the knob - runs the
  // classic loop: pop what is there, run it, and change nothing about when. That is what keeps this file from
  // being a behaviour change anywhere it has not been asked for.
  const bool paced = compat::lane_grid_enabled();

  int64_t tick_ns      = 0;
  int64_t last_tick_ns = 0;
  while (running.load(std::memory_order_relaxed)) {
    unique_task task;
    if (!paced) {
      if (consumer.pop_blocking(task)) {
        task();
        std::lock_guard<std::mutex> lock(stat_mutex);
        ++counters.tasks;
      }
      continue;
    }

    // ---- the elastic band (see the header) ------------------------------------------------------------
    const int64_t period_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(slot_duration).count();
    const int64_t lead_ns   = std::chrono::duration_cast<std::chrono::nanoseconds>(lead).count();
    const int64_t wait_ns   = std::chrono::duration_cast<std::chrono::nanoseconds>(max_wait).count();

    // The tick comes from the radio's grid when it is armed, and from this thread's own clock when it is not -
    // an unarmed grid must degrade to a plain periodic loop rather than to no pacing at all, or a leg whose
    // radio never reports a frontier would silently measure the classic behaviour.
    int64_t next_ns = compat::lane_grid_next_tick_ns(last_tick_ns, period_ns);
    if (next_ns < 0) {
      next_ns = (last_tick_ns == 0) ? (steady_now_ns() + period_ns) : (last_tick_ns + period_ns);
    }
    tick_ns      = next_ns + lead_ns;
    last_tick_ns = next_ns;

    while (running.load(std::memory_order_relaxed) && (steady_now_ns() < tick_ns)) {
      compat::sprint_wait();
    }

    // Poll for work inside the band. A task that is already queued is run at once; one that arrives during the
    // band is run on arrival; and when the band expires the tick is SKIPPED - the task, if it ever comes, runs
    // at the next tick, which is the price of a cadence that does not follow the data.
    const int64_t band_end = tick_ns + wait_ns;
    bool          ran      = false;
    int64_t       ran_at   = 0;
    for (;;) {
      if (consumer.try_pop(task)) {
        if (!ran) {
          ran    = true;
          ran_at = steady_now_ns();
        }
        task();
        {
          std::lock_guard<std::mutex> lock(stat_mutex);
          ++counters.tasks;
        }
        // Drain what else is already there: the stages of one hop arrive as a small burst, and holding the rest
        // for the next tick would add a slot of latency to work that is already in hand.
        continue;
      }
      if (steady_now_ns() >= band_end) {
        break;
      }
      compat::sprint_wait();
    }

    {
      std::lock_guard<std::mutex> lock(stat_mutex);
      ++counters.ticks;
      if (ran) {
        ++counters.ran;
        const int64_t late_us = (ran_at - tick_ns) / 1000;
        counters.late_sum_us += late_us;
        if (late_us > counters.late_max_us) {
          counters.late_max_us = late_us;
        }
      }
      else {
        ++counters.skipped;
      }
    }
  }
}

void paced_task_executor::report() const
{
  const stats s = get_stats();
  if (s.ticks == 0) {
    return; // never paced (the knob was off): print nothing rather than a line of zeroes
  }
  const double mean_us = (s.ran != 0) ? (static_cast<double>(s.late_sum_us) / static_cast<double>(s.ran)) : 0.0;
  std::fprintf(stderr,
               "[paced_exec] %s: ticks=%llu ran=%llu (%.2f%%) skipped=%llu (%.2f%%) tasks=%llu; the first task "
               "of a tick ran %.1fus after its grid instant (max %lldus). A skipped tick is the price of a "
               "cadence that does not follow the data - what it costs the APPLICATION is the number that "
               "decides `max_wait`, not this one\n",
               name.c_str(),
               static_cast<unsigned long long>(s.ticks),
               static_cast<unsigned long long>(s.ran),
               (s.ticks != 0) ? (100.0 * static_cast<double>(s.ran) / static_cast<double>(s.ticks)) : 0.0,
               static_cast<unsigned long long>(s.skipped),
               (s.ticks != 0) ? (100.0 * static_cast<double>(s.skipped) / static_cast<double>(s.ticks)) : 0.0,
               static_cast<unsigned long long>(s.tasks),
               mean_us,
               static_cast<long long>(s.late_max_us));
}
