// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu/support/executors/paced_task_executor.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <unistd.h>

using namespace ocudu;
using namespace std::chrono_literals;

namespace {

/// Waits until \p pred holds or the budget runs out, so a case asserts on a state rather than on a sleep.
template <typename Pred>
bool wait_for(Pred pred, std::chrono::milliseconds budget)
{
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) {
      return true;
    }
    std::this_thread::sleep_for(2ms);
  }
  return pred();
}

} // namespace

/// \brief With the pacing off - the default, and the only behaviour Linux can reach - tasks run and NO tick is
/// counted.
///
/// This is the arm that keeps the executor from changing a leg it was not asked to change: no grid, no pacing,
/// no counters, and the thread behaves like any other single-thread executor.
TEST(paced_task_executor_test, without_the_grid_it_runs_tasks_and_counts_no_ticks)
{
  ::unsetenv("OCUDU_UL_LANE_GRID");
  paced_task_executor exec("paced_test_off", 64, 20ms, 0us, 5ms);

  std::atomic<int> ran{0};
  ASSERT_TRUE(exec.execute([&ran]() { ++ran; }));
  EXPECT_TRUE(wait_for([&ran]() { return ran.load() == 1; }, 500ms)) << "the task must run";
  EXPECT_EQ(exec.get_stats().tasks, 1u);
  EXPECT_EQ(exec.get_stats().ticks, 0u)
      << "with the knob off there is no grid, so a tick counter would be a number about nothing";
}

/// \brief THE abort this file caused on an air leg (p233/p234/p235, `Abort trap: 6`): the object dies before
/// the exit report runs, and a report that still pointed at it locked a mutex inside freed memory.
///
/// The registry is what makes the exit report safe: an executor is IN it while it is alive and OUT of it once
/// it is gone, and `report_all_live()` - the function both exit paths register - can then only ever reach a
/// live object. It is asserted from both sides, because "it did not crash" is exactly the kind of evidence that
/// let the first version through.
TEST(paced_task_executor_test, a_destroyed_executor_is_out_of_the_exit_report_registry)
{
  const size_t before = paced_task_executor::nof_live();
  {
    paced_task_executor exec("paced_test_registry", 64, 20ms, 0us, 2ms);
    EXPECT_EQ(paced_task_executor::nof_live(), before + 1) << "a live executor must be reportable";
  }
  EXPECT_EQ(paced_task_executor::nof_live(), before)
      << "a destroyed executor must be gone from the registry, or the exit report reads freed memory";
  paced_task_executor::report_all_live(); // must be a no-op, not a lock on a dead mutex
}

/// \brief A band longer than the period is CLAMPED, because it would stop the thread keeping one tick per slot.
///
/// This is the sweep's own finding turned into a rule. The air legs asked for 500us, 1ms, 2ms and 4ms bands; the
/// 2ms and 4ms ones were worse on BOTH axes - and a loopback with a 4ms band showed why: the loop ticked 257
/// times a second against the 2000 it was built for, since one "wait" spanned eight slots and the tick
/// structure was simply gone. A wait is only meaningful while it fits inside the period it belongs to.
TEST(paced_task_executor_test, a_band_longer_than_the_period_is_capped_to_it)
{
  paced_task_executor exec("paced_test_cap", 64, 500us, 0us, 4ms);
  EXPECT_EQ(std::chrono::duration_cast<std::chrono::microseconds>(exec.get_max_wait()).count(), 250)
      << "half the period: the wait must end with room left for the drain that follows it";
}

#if defined(__APPLE__)
/// \brief The reading is still TAKEN when the executor dies on a graceful exit - the destructor prints it,
/// because by the time the atexit handlers run the object is already gone (see the registry test above).
///
/// stderr is captured for real here: "the exit path yields a reading" is the entire reason this code exists,
/// and a test that merely did not crash would pass on a version that printed nothing at all.
TEST(paced_task_executor_test, the_destructor_prints_the_reading_a_graceful_exit_would_have_lost)
{
  ::setenv("OCUDU_UL_LANE_GRID", "1", 1);

  std::FILE* tmp = std::tmpfile();
  ASSERT_NE(tmp, nullptr);
  std::fflush(stderr);
  const int saved = ::dup(fileno(stderr));
  ASSERT_GE(saved, 0);
  ASSERT_NE(::dup2(fileno(tmp), fileno(stderr)), -1);

  {
    paced_task_executor exec("paced_test_dtor", 64, 20ms, 0us, 2ms);
    std::atomic<int>   ran{0};
    ASSERT_TRUE(exec.execute([&ran]() { ++ran; }));
    EXPECT_TRUE(wait_for([&ran]() { return ran.load() == 1; }, 500ms));
  } // <- the object dies here, exactly as main's locals do before the atexit handlers run

  std::fflush(stderr);
  ASSERT_NE(::dup2(saved, fileno(stderr)), -1);
  ::close(saved);

  std::rewind(tmp);
  std::string captured;
  char        buf[512];
  size_t      n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), tmp)) != 0) {
    captured.append(buf, n);
  }
  std::fclose(tmp);

  EXPECT_NE(captured.find("[paced_exec] paced_test_dtor"), std::string::npos)
      << "the destructor must print the account, or a graceful air leg has no [paced_exec] line at all";
  EXPECT_NE(captured.find("ticks="), std::string::npos) << "and it must carry the numbers, not just a name";

  ::unsetenv("OCUDU_UL_LANE_GRID");
}

/// \brief THE property of the elastic band: a tick that no task arrives for is SKIPPED and COUNTED.
///
/// It is the difference between this executor and every "delay until the grid instant" variant this line has
/// tried. Those waited indefinitely, so a late arrival was inherited and the cadence followed the data
/// (p224/p225: precise to 3 us, and still 2 ms behind). Here the thread gives up on a tick instead - the slot
/// is lost, the cadence is not - and the count is what a leg reads to see the price.
TEST(paced_task_executor_test, a_tick_with_no_work_is_skipped_not_waited_for)
{
  ::setenv("OCUDU_UL_LANE_GRID", "1", 1);
  ::setenv("OCUDU_UL_LANE_GRID_LEAD_US", "0", 1);

  paced_task_executor exec("paced_test_on", 64, 20ms, 0us, 2ms);

  // One task, then silence. The tick that carries it counts as `ran`; the ticks after it, with nothing to do,
  // must be counted as skipped rather than blocking the loop.
  std::atomic<int> ran{0};
  const auto       pushed_at = std::chrono::steady_clock::now();
  ASSERT_TRUE(exec.execute([&ran]() { ++ran; }));
  EXPECT_TRUE(wait_for([&ran]() { return ran.load() == 1; }, 500ms));
  EXPECT_TRUE(wait_for([&exec]() { return exec.get_stats().skipped >= 3; }, 1000ms))
      << "ticks with nothing to run must be SKIPPED and counted, not waited for: ticks="
      << exec.get_stats().ticks << " skipped=" << exec.get_stats().skipped;

  const auto s = exec.get_stats();
  EXPECT_GE(s.ran, 1u);
  EXPECT_EQ(s.tasks, 1u) << "exactly the one task was run";
  // The bound the band promises: the task ran within one period plus the band plus a generous slack, and
  // certainly not on a wait that follows the data.
  const auto latency = std::chrono::steady_clock::now() - pushed_at;
  EXPECT_LT(latency, 500ms) << "the delay is BOUNDED by the band, which is the whole design";
  EXPECT_GE(s.late_max_us, 0);

  ::unsetenv("OCUDU_UL_LANE_GRID");
  ::unsetenv("OCUDU_UL_LANE_GRID_LEAD_US");
}
#endif

/// \brief N threads share the grid and run AT THE SAME TIME, which is what the pool's concurrency was.
///
/// The first air sweep compared a paced arm that was one thread against a pool that ran two hops concurrently,
/// and read the throughput difference as the price of pacing. It was not: a hop's chain costs more wall time
/// (GPU waits included) than a slot, so one thread cannot sustain the slot rate two could. That is a property
/// of the thread count, not of the pacing - and it is testable without a radio: two tasks that can only finish
/// if they run together.
#if defined(__APPLE__)
TEST(paced_task_executor_test, the_threads_run_at_the_same_time_on_one_grid)
{
  ::setenv("OCUDU_UL_LANE_GRID", "1", 1);
  paced_task_executor exec("paced_test_n", 64, 20ms, 0us, 2ms, 2);

  std::atomic<int> arrived{0};
  std::atomic<int> finished{0};
  auto             rendezvous = [&arrived, &finished]() {
    ++arrived;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (arrived.load() < 2 && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(1ms);
    }
    ++finished;
  };
  ASSERT_TRUE(exec.execute(rendezvous));
  ASSERT_TRUE(exec.execute(rendezvous));
  EXPECT_TRUE(wait_for([&finished]() { return finished.load() == 2; }, 1000ms))
      << "with ONE thread the two tasks would run back to back and the first would sit alone in the "
         "rendezvous until its 2s deadline: arrived="
      << arrived.load() << " finished=" << finished.load();

  ::unsetenv("OCUDU_UL_LANE_GRID");
}
#endif
