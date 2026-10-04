// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu/support/executors/paced_task_executor.h"
#include "ocudu/support/macos_compat.h"
#include "ocudu/support/scheduling/thread_sched_snapshot.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace ocudu;

namespace {

/// The live-instance registry behind `report_all_live()`: heap objects that are NEVER destroyed, because the
/// exit paths that read it run at a point in shutdown where function-local and namespace-scope statics are
/// already being torn down - and a registry that dies before its readers is the same bug one level up.
std::mutex& live_mutex()
{
  static std::mutex* m = new std::mutex();
  return *m;
}

std::vector<const paced_task_executor*>& live_executors()
{
  static std::vector<const paced_task_executor*>* v = new std::vector<const paced_task_executor*>();
  return *v;
}

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
                                         unsigned                         nof_threads_,
                                         os_thread_realtime_priority      prio,
                                         const os_sched_affinity_bitmask& mask) :
  name(std::move(thread_name)),
  slot_duration(slot_duration_),
  lead(lead_),
  // THE BAND IS CAPPED BELOW THE PERIOD (see the header): a wait longer than one slot spans several ticks, and
  // then the thread is not keeping a tick per slot at all. Half the period leaves the rest of the slot for the
  // drain, which is where a burst of a hop's stages goes.
  max_wait(std::min(max_wait_, slot_duration_ / 2)),
  max_wait_requested(max_wait_),
  nof_threads(std::max(1u, nof_threads_)),
  pending(queue_size, std::chrono::microseconds{50})
{
  // The parameter is spelled `nof_threads_` because the member is `nof_threads`: the first version reused the
  // name and Linux's -Werror=shadow refused it - the same class of defect the two-platform build exists to
  // catch, since macOS built it happily.
  {
    std::lock_guard<std::mutex> lock(live_mutex());
    live_executors().push_back(this);
  }
  threads.reserve(nof_threads);
  for (unsigned i = 0; i != nof_threads; ++i) {
    // One thread per index in the name, so a time constraint can be declared per thread (the name is the key).
    const std::string tname = (nof_threads == 1) ? name : (name + "#" + std::to_string(i));
    threads.emplace_back(tname, prio, mask, [this]() { run(); });
  }
}

paced_task_executor::~paced_task_executor()
{
  // THE FLAG ALONE DOES NOT STOP IT, and a test hung for ten minutes to say so: with the pacing off the loop is
  // parked inside `pop_blocking`, which `running` cannot interrupt. A no-op task is pushed after the flag to
  // wake it wherever it is - parked on the queue, or sleeping between two grid ticks - and the join is then
  // bounded by one queue wake-up. (The queued tasks behind it are NOT drained: a leg's stop must not run a
  // backlog it was told to abandon.)
  running.store(false, std::memory_order_relaxed);
  // One wake-up per thread: each is either parked on the queue or sleeping between ticks, and the flag alone
  // interrupts neither (a hung test said so).
  for (size_t i = 0; i != threads.size(); ++i) {
    (void)pending.try_push(unique_task([]() {}));
  }
  for (unique_thread& t : threads) {
    t.join();
  }

  // THE READING IS TAKEN HERE, on the last instant at which this object is certainly alive - and it is taken
  // for exactly the reason the destructor of the mapper's executor list is where an air leg reaches it: after
  // this the object is gone, and an atexit handler that still pointed at it would be reading freed memory
  // (which is what aborted p233/p234/p235).
  if (get_stats().ticks != 0) {
    report();
  }

  // Unregister AFTER the report and after the join: a leg that reports from the exit path must not be able to
  // find an object that is on its way out.
  {
    std::lock_guard<std::mutex> lock(live_mutex());
    auto&                         v = live_executors();
    for (auto it = v.begin(); it != v.end(); ++it) {
      if (*it == this) {
        v.erase(it);
        break;
      }
    }
  }
}

size_t paced_task_executor::nof_live()
{
  std::lock_guard<std::mutex> lock(live_mutex());
  return live_executors().size();
}

void paced_task_executor::report_all_live()
{
  std::lock_guard<std::mutex> lock(live_mutex());
  for (const paced_task_executor* exec : live_executors()) {
    if (exec != nullptr) {
      exec->report();
    }
  }
}

bool paced_task_executor::execute(unique_task task)
{
  return pending.try_push(std::move(task));
}

bool paced_task_executor::defer(unique_task task)
{
  return pending.try_push(std::move(task));
}

bool paced_task_executor::execute_and_wait(unique_task task, std::chrono::nanoseconds timeout)
{
  // EXACTLY ONCE, whichever side runs it. The state machine is the whole mechanism: 0 = nobody has claimed
  // the work yet, 1 = the paced thread has, 2 = the caller has. A claim is taken by compare-and-swap, so a
  // timeout can only ever end in "the caller runs it" or "the thread is running it right now, wait".
  //
  // Why this and not a promise/future: the work may be DROPPED (the queue is full, the executor is stopping),
  // and a broken promise turns that into an exception on a thread that has nothing to do with the failure. It
  // also must not be run TWICE - the caller here commits a Metal command buffer, and committing one twice is
  // not a retry, it is a fault.
  // C++17, so the wait is a condition variable rather than a semaphore - and the predicate is the flag, which
  // is also what makes a spurious wake-up harmless.
  struct handoff {
    std::atomic<int>        claim{0};
    std::mutex              mutex;
    std::condition_variable cv;
    bool                    finished{false};
  };
  auto h = std::make_shared<handoff>();

  const bool queued = pending.try_push(unique_task([h, t = std::move(task)]() mutable {
    int expected = 0;
    if (!h->claim.compare_exchange_strong(expected, 1)) {
      return; // the caller already took it back (it timed out and ran the work itself)
    }
    t();
    {
      std::lock_guard<std::mutex> lock(h->mutex);
      h->finished = true;
    }
    h->cv.notify_all();
  }));
  if (!queued) {
    return false; // nothing was taken: the caller does the work itself
  }

  std::unique_lock<std::mutex> lock(h->mutex);
  if (h->cv.wait_for(lock, timeout, [&h]() { return h->finished; })) {
    return true;
  }
  // The budget is gone. Either the work is still nobody's - in which case the caller takes it back and runs it
  // (that is what the false return means) - or the thread claimed it a moment ago, and then running it here as
  // well would be the second commit of one command buffer. The claim decides, atomically.
  lock.unlock();
  int expected = 0;
  if (h->claim.compare_exchange_strong(expected, 2)) {
    return false;
  }
  lock.lock();
  h->cv.wait(lock, [&h]() { return h->finished; });
  return true;
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

  // ONE LINE SAYS THE THREAD EXISTS AT ALL, and it is not decoration: a loopback meant to verify the paced
  // lane's report printed the banner and then nothing, and "the report is silent" could equally have meant the
  // thread never started, the pacing never engaged, or the object was already gone when the exit path ran. A
  // reading that can be missing for three different reasons is not a reading; this line tells them apart.
  std::fprintf(stderr,
               "[paced_exec] %s: thread up (paced=%d, period=%lldus, band=%lldus) - the tuning knobs were: "
               "OCUDU_UL_PACED_LANE=%s OCUDU_UL_PACED_LEAD_US=%s OCUDU_UL_PACED_WAIT_US=%s\n",
               name.c_str(),
               paced ? 1 : 0,
               static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(slot_duration).count()),
               static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(max_wait).count()),
               std::getenv("OCUDU_UL_PACED_LANE") != nullptr ? std::getenv("OCUDU_UL_PACED_LANE") : "(unset)",
               std::getenv("OCUDU_UL_PACED_LEAD_US") != nullptr ? std::getenv("OCUDU_UL_PACED_LEAD_US") : "(unset)",
               std::getenv("OCUDU_UL_PACED_WAIT_US") != nullptr ? std::getenv("OCUDU_UL_PACED_WAIT_US") : "(unset)");

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
    // RE-SYNC WHEN THE PREVIOUS ITERATION OVERRAN, and this is not a detail - the air sweep measured what
    // happens without it. The next tick is computed from the PREVIOUS TICK, so a loop that spent a whole band
    // per iteration advanced its tick by one period while taking the band's time; with band > period the tick
    // fell further behind every round and the grid's phase was lost for the rest of the leg. Asking "the first
    // tick still ahead of NOW" instead keeps the phase: a tick that is in the past is never one to run.
    const int64_t now_ns = steady_now_ns();
    if (next_ns + lead_ns <= now_ns) {
      const int64_t resynced = compat::lane_grid_next_tick_ns(now_ns, period_ns);
      next_ns                = (resynced < 0) ? (now_ns + period_ns) : resynced;
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
      // THE BAND IS A DEADLINE FOR WAITING, NOT A PERIOD TO FILL. Once the work has run there is nothing left
      // to wait for, and staying in the band only costs the tick's phase (see the re-sync above). The first
      // version polled to the end of the band either way, and the sweep's larger bands show what that buys:
      // more retransmissions for less goodput.
      if (ran) {
        break;
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
  // A ZERO IS PRINTED, NOT SWALLOWED. This used to return early "rather than print a line of zeroes", and that
  // silence cost a loopback: the report was missing and there was no way to tell a registry that dropped it,
  // a thread that never started, and a run that simply had no ticks. A leg is read by a person, and "0 ticks"
  // is a fact about the leg - the only thing that must never happen is not knowing.
  if (s.ticks == 0) {
    std::fprintf(stderr,
                 "[paced_exec] %s: ticks=0 - the loop never paced (knob off, or the thread never ran). This "
                 "line exists so that a missing READING and a missing NUMBER cannot be confused.\n",
                 name.c_str());
    return;
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
  const auto req_us = std::chrono::duration_cast<std::chrono::microseconds>(max_wait_requested).count();
  const auto eff_us = std::chrono::duration_cast<std::chrono::microseconds>(max_wait).count();
  const auto per_us = std::chrono::duration_cast<std::chrono::microseconds>(slot_duration).count();
  const auto lead_us = std::chrono::duration_cast<std::chrono::microseconds>(lead).count();
  std::fprintf(stderr,
               "[paced_exec] %s: period=%lldus lead=%lldus band=%lldus%s threads=%u; ticks above are the SUM "
               "over the threads, so divide by %u for a per-thread rate\n",
               name.c_str(),
               static_cast<long long>(per_us),
               static_cast<long long>(lead_us),
               static_cast<long long>(eff_us),
               (eff_us != req_us) ? " (CLAMPED below the period: a longer wait spans several ticks, so the "
                                    "thread would no longer keep one tick per slot)"
                                  : "",
               nof_threads,
               nof_threads);
}
