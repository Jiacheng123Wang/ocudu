// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace ocudu {

/// \brief A 1 ms waker that classifies the stalls a leg contains into the three kinds that look identical.
///
/// WHY IT EXISTS (plan doc phy_thread_scheduling_plan.md D/D.1, high level 4bis unknown #1). Every stall this
/// workstream has measured leaves the SAME fingerprint - the process's CPU falls to 0.1-0.5 cores and its
/// involuntary context-switch rate falls to a tenth of the leg's own baseline - and that fingerprint has
/// three mutually exclusive causes:
///
///   1. the whole process was suspended (there was nothing runnable);
///   2. the stalled threads were BLOCKED in a driver or the kernel (again nothing runnable);
///   3. the threads were RUNNABLE and lost the CPU to someone else.
///
/// Only (3) is something a scheduling mechanism can address, so without this classification every lever
/// decision is blind, and a negative result (the time constraint did not help on seven pairs) cannot be
/// explained - only reported. That distinction is the whole point: "the constraint fixes a class of stall
/// that is not what hurts us" generalizes to other machines, "the constraint does not work here" does not.
///
/// HOW IT DECIDES. The watchdog wakes every 1 ms and records how LATE it was. Being late means the machine
/// was not running us either, so the system's own busyness decides between (1) and saturation. When it is
/// ON TIME but a measured series has just stalled, the stalled threads' own run states decide: blocked (2)
/// versus runnable-but-not-running (3) versus running, i.e. the work itself was slow. Those states come from
/// thread_info(THREAD_EXTENDED_INFO) on the thread ports that task_threads() returns, and the system's
/// busyness from host_statistics(HOST_CPU_LOAD_INFO) - which is millisecond-scale, unlike load1, whose
/// 60-second average cannot see a 10 ms event at all (discipline 78).
///
/// KEYS AND COST, like every instrument here: compiled only with OCUDU_FLOW_PROBES and started only when
/// OCUDU_UL_WATCHDOG is set to something other than 0. With either key off, start_if_enabled() reads one
/// environment variable and returns - no thread, no clock, no output. Sampling is gated on suspicion rather
/// than done every tick, because this project has already paid once for an observer perturbing what it
/// observes (2.4).
///
/// \note It cannot say WHICH syscall or driver a blocked thread is in - that needs a stack sample, which
///       perturbs. What it gives is the instant at which such a sample is worth taking, and the tally that
///       says whether taking one is worth it at all.
class ul_stall_watchdog
{
public:
  static ul_stall_watchdog& get();

  /// Starts the 1 ms waker once per process, if both keys are on. Cheap to call repeatedly and from any
  /// thread (worker creation calls it once per worker).
  void start_if_enabled();

  /// Called by the probe when a measured series files a tail event, so an ON-TIME watchdog still gets to
  /// classify a stall. Ignored when the watchdog is off.
  void notify_series_stall(int64_t value_us);

  /// Prints the histogram and the classified stalls. Does nothing when the knob is off, so a delivery leg's
  /// report stays byte-identical.
  void report();

  /// Test hooks. The classification is arithmetic over four numbers and a trigger, so it is testable without
  /// a machine: feed it the numbers, read the verdict.
  void reset_for_test();
  void tick_for_test(int64_t lateness_ns);
  const char* classify_for_test(bool watchdog_late, int sys_busy_pct, int n_running, int n_runnable, int n_blocked);

  /// Whether \p thread_name belongs to the set the verdict is computed over. Exposed because a filter that is
  /// wrong in the permissive direction silently restores the old "any parked thread counts" verdict, and one
  /// wrong in the strict direction yields an empty table - both look like "no stalls" in a leg.
  static bool is_watched_thread_for_test(const char* thread_name);
  /// Overflow/robustness hook: a tick far beyond any real stall must land in the top bucket, not outside it.
  void inject_late_for_test(int64_t extra_ns);

private:
  ul_stall_watchdog()  = default;
  ~ul_stall_watchdog() = default;

  struct impl;
  impl* p = nullptr;
};

} // namespace ocudu
