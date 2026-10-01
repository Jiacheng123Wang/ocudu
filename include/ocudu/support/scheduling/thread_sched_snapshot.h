// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/support/executors/unique_thread.h" // os_thread_realtime_priority
#include <cstdint>
#include <string_view>

namespace ocudu {

/// \brief A reading of the CALLING thread's own CPU and scheduling state.
///
/// WHY THIS EXISTS (doc_chinese/macos_thread_priority/thread_priority_optimization_design_and_implementation.md,
/// stage P1). Every instrument this project had before it measured the PROCESS: `[ul_timing_events]` asks
/// getrusage(RUSAGE_SELF) for the CPU the process consumed during a window, and `ps -M`/`taskinfo` describe the
/// process from outside. That cannot answer the question this workstream is about, because the two owners of a
/// millisecond stall look identical from there:
///
///   * "the OS took the core away from THIS thread while the process kept running" - a scheduling problem, and
///     the one macOS can actually have, since it has no hard real time (high level 4); and
///   * "the whole process was off-CPU" / "the thread was on-CPU for the whole window and the work itself was
///     slow" - two different stories again.
///
/// A per-thread CPU reading separates them: a thread that was descheduled shows ~0 CPU over a window in which
/// the process as a whole consumed milliseconds.
///
/// WHAT THE PLATFORMS CAN AND CANNOT PROVIDE (measured against the SDKs, 2026-10-01):
/// | field | macOS | Linux |
/// |---|---|---|
/// | cpu_ns | THREAD_BASIC_INFO (user_time + system_time, microsecond time_value_t) | RUSAGE_THREAD (utime+stime) |
/// | nvcsw / ivcsw | **not available**: no Mach thread-info flavor carries context-switch counts | RUSAGE_THREAD (ru_nvcsw / ru_nivcsw) |
/// | run_state | THREAD_BASIC_INFO.run_state (TH_STATE_*) | 0 (no equivalent) |
/// | qos_class | pthread_get_qos_class_np | -1 (no equivalent notion) |
/// | posix_policy / posix_prio | pthread_getschedparam (recorded by the kernel, not enforced) | pthread_getschedparam (ENFORCED) |
///
/// The unavailable fields are -1, never 0: this project's rule is that "no reading" must not be printable as
/// "zero" (dev doc phy_latency 6.244/6.245, the `cpu=-` refusal).
struct thread_sched_snapshot {
  /// The calling thread's cumulative CPU time (user + system), in nanoseconds. -1 = the platform refused.
  int64_t cpu_ns = -1;
  /// Voluntary context switches of the calling thread. -1 = the platform does not expose them (macOS).
  int64_t nvcsw = -1;
  /// Involuntary context switches of the calling thread. -1 = the platform does not expose them (macOS).
  int64_t ivcsw = -1;
  /// Monotonic instant the reading was taken, in nanoseconds (the same clock the pipeline probes use). It is
  /// what lets two snapshots say how much CPU the thread consumed over a window.
  int64_t wall_ns = 0;
  /// macOS: THREAD_BASIC_INFO.run_state (1 = running, 2 = runnable, 3 = blocked, ...). Linux: 0.
  int32_t run_state = 0;
  /// macOS: the QoS class the thread is ACTUALLY in (pthread_get_qos_class_np), which is what says whether the
  /// class the code requested was granted or clamped. -1 = not applicable.
  int32_t qos_class = -1;
  /// POSIX scheduling policy as read back (SCHED_OTHER / SCHED_FIFO / SCHED_RR). -1 = the query failed.
  int32_t posix_policy = -1;
  /// POSIX scheduling priority as read back.
  int32_t posix_prio = -1;
  /// The Mach time constraint (THREAD_TIME_CONSTRAINT_POLICY) the thread is ACTUALLY under, read back with
  /// thread_policy_get. > 0 = constrained, 0 = the read succeeded and there is no explicit constraint, -1 = the
  /// platform has no such notion or the query failed.
  ///
  /// WHY IT IS IN THIS SNAPSHOT: a time constraint is NOT additive on Darwin - applying one ERASES the thread's
  /// QoS class, irreversibly (measured 2026-10-01, dev doc 10.29: `set_qos(UI)` -> qos 33, then
  /// thread_policy_set(TIME_CONSTRAINT) -> qos 0, and setting it again returns EPERM). A readback line that
  /// shows only `eff=` would therefore describe an arm that does not exist: a thread under a constraint can
  /// never be `eff=USER_INTERACTIVE` at the same time.
  int64_t tc_period_ns      = -1;
  int64_t tc_computation_ns = -1;
  int64_t tc_constraint_ns  = -1;
  /// True when this platform can read a time constraint at all (macOS). On Linux the three tc_* fields stay -1,
  /// and this accessor is the ONLY safe way to tell that apart from "read succeeded, no constraint" (0): the
  /// fields are nanoseconds, so a consumer that converts to microseconds with integer division turns -1 into 0
  /// and erases the distinction - which is what the Ubuntu bench caught on 2026-10-01 (dev doc 10.29).
  bool tc_readable() const { return tc_period_ns >= 0; }
  /// True when the kernel read back an explicit time constraint on this thread.
  bool time_constrained() const { return tc_period_ns > 0; }
  /// The nominal duty cycle the thread declares to the kernel (computation / period). 0 when unconstrained.
  /// It is the number that matters for calibration: this is CPU the kernel will let the thread take.
  double declared_duty() const
  {
    return (tc_period_ns > 0) ? (static_cast<double>(tc_computation_ns) / static_cast<double>(tc_period_ns)) : 0.0;
  }
  /// The platform thread id (macOS: pthread_threadid_np; Linux: gettid), 0 when it could not be read. It is
  /// what makes two snapshots comparable AT ALL: subtracting one thread's CPU from another's baseline is the
  /// mistake this field exists to refuse (see the probe's `tcpu=`).
  uint64_t thread_id = 0;
  /// True when the CPU reading itself is valid (a platform may expose the rest and not this).
  bool valid() const { return cpu_ns >= 0; }
};

/// \brief Reads the calling thread's own CPU and scheduling state.
///
/// Cheap on both platforms (one Mach call / one syscall) but never free, so callers take it only where a
/// reading is actually kept - exactly like the process-wide baseline the timing events already use.
thread_sched_snapshot this_thread_sched_snapshot();

/// \brief Reads ONLY the calling thread's cumulative CPU, in nanoseconds (-1 when the platform refuses).
///
/// WHY IT IS SEPARATE: this_thread_sched_snapshot() answers "what is this thread's scheduling state", and it pays
/// for that answer with five calls (pthread_getschedparam, pthread_threadid_np, pthread_get_qos_class_np,
/// thread_info, thread_policy_get). A caller that only wants the CPU counter - the per-slot accounting that
/// calibrates a Mach time constraint's `computation` (dev doc 10.30(8)) - must not pay for the other four on
/// every pipeline landmark, so this is the same reading with nothing else attached.
int64_t this_thread_cpu_ns();

/// \brief Returns a printable name for a qos_class value ("USER_INTERACTIVE", "-", ...).
const char* qos_class_name(int32_t qos);

/// \brief Returns a printable name for a run_state value ("running", "blocked", ...).
const char* thread_run_state_name(int32_t run_state);

/// \brief Prints ONE line describing what the fresh worker thread actually got, when OCUDU_SCHED_VERBOSE is on.
///
/// This is the instrument that answers the open question of high level 4: the code REQUESTS a QoS class and a
/// POSIX priority, and nothing ever read back what the kernel granted - `taskinfo` showed UI/IN QoS billing of
/// 0 s with an effective ceiling of THREAD_QOS_LEGACY, which is consistent with "requested, recorded, clamped"
/// and with three other stories (dev doc 10.1). One loopback run with this switch on separates them.
///
/// Two keys, like every instrument here: it is compiled only when OCUDU_FLOW_PROBES is defined, and it prints
/// only when the environment variable OCUDU_SCHED_VERBOSE is set to something other than "0". With either key
/// off it prints nothing at all, on BOTH platforms, so a leg's report is byte-identical.
///
/// \param[in] prio        The real-time priority intent the thread was created with (what we asked for).
/// \param[in] thread_name The worker name (printed because the pthread name is set just before this call, and a
///                        line that names the thread is the only kind this project accepts as evidence).
void log_this_thread_scheduling(const os_thread_realtime_priority& prio, std::string_view thread_name);

} // namespace ocudu
