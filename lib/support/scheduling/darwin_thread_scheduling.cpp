// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu/support/scheduling/darwin_thread_scheduling.h"
#include "ocudu/support/executors/thread_utils.h" // this_thread_name()
#include <atomic>
#include <cstdio>

using namespace ocudu;

#if defined(__APPLE__)

#include <mach/mach.h>
#include <mach/mach_time.h> // mach_timebase_info: note that <mach/mach.h> does not pull this in.
#include <mach/thread_policy.h>
#include <pthread.h>

os_qos_class_t ocudu::darwin_qos_class_for_prio(os_thread_realtime_priority prio)
{
  return (prio == os_thread_realtime_priority::no_realtime()) ? QOS_CLASS_USER_INITIATED
                                                              : QOS_CLASS_USER_INTERACTIVE;
}

void ocudu::set_this_thread_qos_class(os_qos_class_t qos)
{
  // Must be called from within the target thread. relative_priority 0 keeps the default band of the class.
  if (::pthread_set_qos_class_self_np(qos, 0) != 0) {
    std::fprintf(stderr, "Warning: failed to set QoS class %u for thread \"%s\".\n", unsigned(qos), this_thread_name());
  }
}

void ocudu::set_pthread_attr_qos_class(::pthread_attr_t& attr, os_qos_class_t qos)
{
  // The kernel's QoS policy overrides plain POSIX scheduling parameters: declare the class on the attributes so the
  // thread starts on the performance cores from its first instruction.
  if (::pthread_attr_set_qos_class_np(&attr, qos, 0) != 0) {
    std::fprintf(stderr, "Warning: failed to set QoS class %u on a thread attribute.\n", unsigned(qos));
  }
}

void ocudu::set_this_thread_time_constraint(const darwin_thread_time_constraint& constraint)
{
  // Conversion factor from microseconds to Mach absolute time units (timebase-dependent; 1:1 on Apple Silicon,
  // e.g., 24 MHz ticks on some models). mach_timebase_info reports numer/denom nanoseconds per tick, so
  // ticks = 1000 * us * denom / numer.
  static const double abs_ticks_per_us = []() {
    ::mach_timebase_info_data_t timebase_info;
    ::mach_timebase_info(&timebase_info);
    return 1000.0 * static_cast<double>(timebase_info.denom) / static_cast<double>(timebase_info.numer);
  }();

  const auto to_abs_ticks = [](std::chrono::microseconds us) {
    return static_cast<uint32_t>(static_cast<double>(us.count()) * abs_ticks_per_us);
  };

  thread_time_constraint_policy_data_t policy = {};
  policy.period                              = to_abs_ticks(constraint.period);
  policy.computation                         = to_abs_ticks(constraint.computation);
  policy.constraint                          = to_abs_ticks(constraint.constraint);
  policy.preemptible                         = constraint.preemptible;

  ::kern_return_t kr = ::thread_policy_set(::mach_thread_self(),
                                           THREAD_TIME_CONSTRAINT_POLICY,
                                           reinterpret_cast<thread_policy_t>(&policy),
                                           THREAD_TIME_CONSTRAINT_POLICY_COUNT);
  if (kr != KERN_SUCCESS) {
    // The constraint is attempted on every real-time worker: report the failure only once per process.
    static std::atomic<bool> warned{false};
    if (not warned.exchange(true)) {
      std::fprintf(stderr,
                   "Warning: failed to set Mach time constraint on thread \"%s\" (kern_return %d). "
                   "The QoS class remains in effect.\n",
                   this_thread_name(),
                   int(kr));
    }
  }
}

void ocudu::set_this_thread_affinity_tag(int affinity_tag)
{
#if defined(__x86_64__)
  // THREAD_AFFINITY_POLICY (L2-cluster co-location hint) is implemented by XNU on Intel only. On Apple Silicon it
  // is not supported: thread_policy_set() returns KERN_NOT_SUPPORTED (46) for this policy, since the arm64 AMP
  // scheduler does not expose L2 grouping to user space. On arm64 the QoS class set above is the supported mechanism
  // steering threads to the performance cores; the affinity tag is therefore computed but unused (see below).
  thread_affinity_policy_data_t policy = {};
  policy.affinity_tag                  = affinity_tag;

  ::kern_return_t kr = ::thread_policy_set(::mach_thread_self(),
                                           THREAD_AFFINITY_POLICY,
                                           reinterpret_cast<thread_policy_t>(&policy),
                                           THREAD_AFFINITY_POLICY_COUNT);
  if (kr != KERN_SUCCESS) {
    std::fprintf(stderr,
                 "Warning: failed to set thread affinity tag %d for thread \"%s\" (kern_return %d).\n",
                 affinity_tag,
                 this_thread_name(),
                 int(kr));
  }
#else
  (void)affinity_tag;
#endif
}

#else // not __APPLE__: stub implementations so the module can be compiled everywhere.

os_qos_class_t ocudu::darwin_qos_class_for_prio(os_thread_realtime_priority /*prio*/)
{
  return 0;
}

void ocudu::set_this_thread_qos_class(os_qos_class_t /*qos*/) {}

void ocudu::set_pthread_attr_qos_class(::pthread_attr_t& /*attr*/, os_qos_class_t /*qos*/) {}

void ocudu::set_this_thread_time_constraint(const darwin_thread_time_constraint& /*constraint*/) {}

void ocudu::set_this_thread_affinity_tag(int /*affinity_tag*/) {}

#endif
// The definitions of affinity_tag_from_cpu_mask() and affinity_tag_from_thread_name() live in the
// macos_compat target (utils/macos_compat/macos_compat.cpp): they are shared by the compat wrappers
// and this module, and that placement avoids a link cycle with ocudu_support.
