// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
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
  // e.g., 24 MHz ticks on some models).
  static const double abs_ticks_per_us = []() {
    ::mach_timebase_info_data_t timebase_info;
    ::mach_timebase_info(&timebase_info);
    return static_cast<double>(timebase_info.denom) / static_cast<double>(timebase_info.numer);
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

int ocudu::affinity_tag_from_cpu_mask(const os_sched_affinity_bitmask& mask)
{
  if (not mask.any() || mask.count() > 4) {
    return 0;
  }
  return mask.find_lowest(0, mask.size()) + 1;
}

int ocudu::affinity_tag_from_thread_name(std::string_view name)
{
  // Strip the worker index suffix: "main_pool#2" -> "main_pool".
  std::string_view pool_prefix = name;
  if (auto pos = pool_prefix.rfind('#'); pos != std::string_view::npos) {
    pool_prefix = pool_prefix.substr(0, pos);
  }

  // FNV-1a, folded into [1, 4095] (0 is reserved for "no hint"). With ~10 distinct pools, collisions are negligible.
  uint32_t hash = 2166136261u;
  for (char c : pool_prefix) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 16777619u;
  }
  return 1 + static_cast<int>(hash % 4095);
}
