// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu/support/scheduling/thread_sched_snapshot.h"
#include "ocudu/support/scheduling/darwin_thread_scheduling.h" // darwin_qos_class_for_prio
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <sched.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>    // mach_timebase_info: ticks -> ns for the time-constraint readback
#include <mach/thread_info.h>
#include <mach/thread_policy.h> // THREAD_TIME_CONSTRAINT_POLICY readback
#include <pthread/qos.h>
#elif !defined(_WIN32)
#include <sys/resource.h> // getrusage(RUSAGE_THREAD): the per-thread reading Linux has and Darwin does not
#include <sys/syscall.h>
#include <unistd.h>
#endif

using namespace ocudu;

namespace {

/// Monotonic nanoseconds, the clock the pipeline probes already use (so a snapshot's instant can be compared
/// with an event's `begin_ns`/`end_ns` without converting anything).
int64_t steady_now_ns()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

#if defined(__APPLE__)
/// The calling thread's Mach port name. mach_thread_self() returns a SEND RIGHT that must be released, and the
/// two readers below both need it, so the release is done once by the caller's scope guard.
struct mach_thread_port {
  mach_port_t port = MACH_PORT_NULL;
  mach_thread_port() : port(::mach_thread_self()) {}
  ~mach_thread_port()
  {
    if (port != MACH_PORT_NULL) {
      ::mach_port_deallocate(::mach_task_self(), port);
    }
  }
  mach_thread_port(const mach_thread_port&)            = delete;
  mach_thread_port& operator=(const mach_thread_port&) = delete;
};
#endif

} // namespace

thread_sched_snapshot ocudu::this_thread_sched_snapshot()
{
  thread_sched_snapshot snap;
  snap.wall_ns = steady_now_ns();

  int policy = -1;
  sched_param param{};
  if (::pthread_getschedparam(::pthread_self(), &policy, &param) == 0) {
    snap.posix_policy = policy;
    snap.posix_prio   = param.sched_priority;
  }

#if defined(__APPLE__)
  snap.thread_id = 0;
  uint64_t tid   = 0;
  if (::pthread_threadid_np(nullptr, &tid) == 0) {
    snap.thread_id = tid;
  }
  qos_class_t qos = QOS_CLASS_UNSPECIFIED;
  int         rel = 0;
  if (::pthread_get_qos_class_np(::pthread_self(), &qos, &rel) == 0) {
    snap.qos_class = static_cast<int32_t>(qos);
  }
  // THREAD_BASIC_INFO carries the thread's own CPU as two time_value_t (seconds + microseconds) and the run
  // state. This is the only per-thread CPU reading macOS exposes; note that it is NOT the same counter as
  // getrusage(RUSAGE_SELF), so the two must never be subtracted from one another.
  mach_thread_port                port;
  thread_basic_info_data_t        basic{};
  mach_msg_type_number_t          basic_count = THREAD_BASIC_INFO_COUNT;
  if (::thread_info(port.port,
                    THREAD_BASIC_INFO,
                    reinterpret_cast<thread_info_t>(&basic),
                    &basic_count) == KERN_SUCCESS) {
    snap.cpu_ns = (static_cast<int64_t>(basic.user_time.seconds) + static_cast<int64_t>(basic.system_time.seconds)) *
                      1000000000LL +
                  (static_cast<int64_t>(basic.user_time.microseconds) +
                   static_cast<int64_t>(basic.system_time.microseconds)) *
                      1000LL;
    snap.run_state = static_cast<int32_t>(basic.run_state);
    if (snap.posix_policy < 0) {
      snap.posix_policy = static_cast<int32_t>(basic.policy);
    }
  }
  // nvcsw/ivcsw stay -1: no Mach thread-info flavor exposes context-switch counts per thread (checked against
  // the SDK: thread_basic_info has run_state/sleep_time/flags, thread_extended_info adds priorities and the
  // name, and neither has a switch counter). The process-wide counts from getrusage(RUSAGE_SELF) are what the
  // timing events print instead, and this refusal is why they are labeled process-wide.
  //
  // The Mach time constraint IS readable, but only with the trick that cost me a wrong probe earlier today:
  // thread_policy_get()'s get_default argument is IN/OUT and must be FALSE on input, otherwise the call reports
  // the kernel's default (0/120000/240000) and a constrained thread looks unconstrained. With FALSE it reports
  // the thread's real policy and sets get_default on output when there is none.
  {
    mach_thread_port                     tc_port;
    thread_time_constraint_policy_data_t tc{};
    mach_msg_type_number_t               tc_count   = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
    boolean_t                            tc_default = FALSE;
    if (::thread_policy_get(tc_port.port,
                            THREAD_TIME_CONSTRAINT_POLICY,
                            reinterpret_cast<thread_policy_t>(&tc),
                            &tc_count,
                            &tc_default) == KERN_SUCCESS) {
      if (tc_default || (tc.period == 0)) {
        snap.tc_period_ns      = 0;
        snap.tc_computation_ns = 0;
        snap.tc_constraint_ns  = 0;
      }
      else {
        // Mach absolute time ticks, converted with the machine's own timebase (24 MHz => 125/3 ns per tick on
        // Apple Silicon), the same conversion darwin_thread_scheduling.cpp applies in the other direction.
        static const double ns_per_tick = [] {
          mach_timebase_info_data_t tb{};
          ::mach_timebase_info(&tb);
          return (tb.denom != 0) ? (static_cast<double>(tb.numer) / static_cast<double>(tb.denom)) : 1.0;
        }();
        snap.tc_period_ns = static_cast<int64_t>(static_cast<double>(tc.period) * ns_per_tick);
        snap.tc_computation_ns = static_cast<int64_t>(static_cast<double>(tc.computation) * ns_per_tick);
        snap.tc_constraint_ns = static_cast<int64_t>(static_cast<double>(tc.constraint) * ns_per_tick);
      }
    }
  }
#elif !defined(_WIN32)
  snap.thread_id = static_cast<uint64_t>(::syscall(SYS_gettid));
  rusage ru{};
  if (::getrusage(RUSAGE_THREAD, &ru) == 0) {
    snap.cpu_ns = (static_cast<int64_t>(ru.ru_utime.tv_sec) + static_cast<int64_t>(ru.ru_stime.tv_sec)) *
                      1000000000LL +
                  (static_cast<int64_t>(ru.ru_utime.tv_usec) + static_cast<int64_t>(ru.ru_stime.tv_usec)) * 1000LL;
    snap.nvcsw = static_cast<int64_t>(ru.ru_nvcsw);
    snap.ivcsw = static_cast<int64_t>(ru.ru_nivcsw);
  }
#endif
  return snap;
}

const char* ocudu::qos_class_name(int32_t qos)
{
#if defined(__APPLE__)
  switch (qos) {
    case QOS_CLASS_USER_INTERACTIVE:
      return "USER_INTERACTIVE";
    case QOS_CLASS_USER_INITIATED:
      return "USER_INITIATED";
    case QOS_CLASS_DEFAULT:
      return "DEFAULT";
    case QOS_CLASS_UTILITY:
      return "UTILITY";
    case QOS_CLASS_BACKGROUND:
      return "BACKGROUND";
    case QOS_CLASS_UNSPECIFIED:
      return "UNSPECIFIED";
    default:
      break;
  }
#endif
  (void)qos;
  return "-";
}

const char* ocudu::thread_run_state_name(int32_t run_state)
{
#if defined(__APPLE__)
  switch (run_state) {
    case TH_STATE_RUNNING:
      return "running";
    case TH_STATE_STOPPED:
      return "stopped";
    case TH_STATE_WAITING:
      return "waiting";
    case TH_STATE_UNINTERRUPTIBLE:
      return "uninterruptible";
    case TH_STATE_HALTED:
      return "halted";
    default:
      break;
  }
#endif
  (void)run_state;
  return "-";
}

void ocudu::log_this_thread_scheduling(const os_thread_realtime_priority& prio, std::string_view thread_name)
{
#if defined(OCUDU_FLOW_PROBES)
  // Key 1 (compile time) is the #if above; key 2 (run time) is this variable. Both must be on: the first keeps
  // the readback out of a production build, the second keeps it out of a leg's report. With either off this
  // function reads one environment variable per thread creation and returns - no clock, no Mach call, no line.
  const char* env = std::getenv("OCUDU_SCHED_VERBOSE");
  if ((env == nullptr) || (env[0] == '\0') || ((env[0] == '0') && (env[1] == '\0'))) {
    return;
  }
  const thread_sched_snapshot snap      = this_thread_sched_snapshot();
  const bool                  rt_intent = (prio != os_thread_realtime_priority::no_realtime());
#if defined(__APPLE__)
  const char* requested = qos_class_name(static_cast<int32_t>(darwin_qos_class_for_prio(prio)));
#else
  // Linux has no QoS notion: the requested priority IS the POSIX one, and it is enforced here.
  const char* requested = "posix";
#endif
  const char* effective = (snap.qos_class >= 0) ? qos_class_name(snap.qos_class) : "n/a";
  const char* policy    = "-";
  switch (snap.posix_policy) {
    case SCHED_OTHER:
      policy = "OTHER";
      break;
#if defined(SCHED_FIFO)
    case SCHED_FIFO:
      policy = "FIFO";
      break;
#endif
#if defined(SCHED_RR)
    case SCHED_RR:
      policy = "RR";
      break;
#endif
    default:
      break;
  }
  // ONE line per worker thread, printed from INSIDE the thread and AFTER every scheduling call the thread
  // wrapper makes (unique_thread: apply_worker_thread_scheduling -> pthread_setschedparam -> affinity). Reading
  // it back anywhere earlier reports a state the thread has not reached yet - which is exactly how "requested"
  // and "effective" get confused for one another.
  //
  // `req=` is what the code asked for (the QoS class the priority intent maps to); `eff=` is what the kernel
  // says the thread is in. They differ when the request was clamped, and that difference is the answer this
  // instrument exists to produce (high level 4: UI/IN billing 0 s with an effective ceiling of THREAD_QOS_LEGACY).
  // `tc=` is the third key fact about the arm: on Darwin a Mach time constraint and a QoS class are mutually
  // exclusive and the constraint wins (it erases the class), so `eff=UNSPECIFIED` next to `tc=-` means "we lost
  // the class to something else" while `eff=UNSPECIFIED` next to `tc=1000/100/200us` means "this thread is
  // deliberately on a budget". Those two are the same string without this field, and they are different arms.
  char tc_str[64];
  if (snap.tc_period_ns < 0) {
    std::snprintf(tc_str, sizeof(tc_str), "-");
  }
  else if (!snap.time_constrained()) {
    std::snprintf(tc_str, sizeof(tc_str), "none");
  }
  else {
    std::snprintf(tc_str,
                  sizeof(tc_str),
                  "%lld/%lld/%lldus(duty=%.0f%%)",
                  static_cast<long long>(snap.tc_period_ns / 1000),
                  static_cast<long long>(snap.tc_computation_ns / 1000),
                  static_cast<long long>(snap.tc_constraint_ns / 1000),
                  100.0 * snap.declared_duty());
  }
  std::fprintf(stderr,
               "[sched] thread=%-16s id=%llu rt_intent=%d req=%s eff=%s run=%s posix=%s/%d tc=%s cpu=%.3fms%s\n",
               std::string(thread_name).c_str(),
               static_cast<unsigned long long>(snap.thread_id),
               rt_intent ? 1 : 0,
               requested,
               effective,
               thread_run_state_name(snap.run_state),
               policy,
               snap.posix_prio,
               tc_str,
               static_cast<double>(snap.cpu_ns >= 0 ? snap.cpu_ns : 0) / 1e6,
               // A thread that has just been created has burned ~0 CPU, so `cpu=` is not a reading of anything
               // yet. It is printed because a MISSING field would look like a broken instrument, and because a
               // thread created after startup (a pool respawn) is worth telling apart by its own number.
               snap.valid() ? "" : " (no per-thread CPU reading on this platform)");
#endif // OCUDU_FLOW_PROBES
}
