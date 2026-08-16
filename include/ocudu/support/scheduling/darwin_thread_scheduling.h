// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/support/executors/unique_thread.h" // os_thread_realtime_priority, os_sched_affinity_bitmask
#include <chrono>
#include <pthread.h>
#include <string_view>

#if defined(__APPLE__)
// pthread_set_qos_class_self_np and the QoS type/constants. Note that the public declaration lives in
// <pthread/qos.h> (included by <pthread.h>); include it explicitly so the API is found on all SDK versions.
#include <pthread/qos.h>
#endif

namespace ocudu {

/// OS thread QoS class. Type-erases \c qos_class_t so that the header compiles on all platforms.
using os_qos_class_t =
#if defined(__APPLE__)
    qos_class_t;
#else
    unsigned;
#endif

/// \brief Maps the POSIX real-time priority intent to a Darwin QoS class.
///
/// The gNB worker configuration expresses criticality through \c os_thread_realtime_priority: data-plane and PHY
/// executors (main pool, lower PHY, io_broker) are created with a real-time priority, while supporting workers use
/// \c no_realtime(). On Darwin, SCHED_FIFO cannot be set without privileges; the QoS class is the closest equivalent:
/// - real-time intent -> QOS_CLASS_USER_INTERACTIVE (latency-critical, scheduled on P-cores),
/// - otherwise       -> QOS_CLASS_USER_INITIATED (compute-bound, P-cores, slightly lower latency budget).
os_qos_class_t darwin_qos_class_for_prio(os_thread_realtime_priority prio);

/// \brief Sets the QoS class of the calling thread (pthread_set_qos_class_self_np). No-op off Darwin.
void set_this_thread_qos_class(os_qos_class_t qos);

/// \brief Sets the QoS class on a pthread attribute, applied at thread creation (pthread_attr_set_qos_class_np).
///
/// On macOS the kernel's QoS policy overrides plain POSIX scheduling parameters (pthread_setschedparam), so the
/// class must be declared on the attributes before pthread_create: the thread then starts on the performance cores.
/// No-op off Darwin.
void set_pthread_attr_qos_class(::pthread_attr_t& attr, os_qos_class_t qos);

/// \brief Mach real-time time constraint parameters (THREAD_TIME_CONSTRAINT_POLICY).
///
/// The thread is expected to need \c computation of CPU time per \c period and must receive it within
/// \c constraint from the start of the period. A non-preemptible thread is not interrupted by lower-priority work
/// (e.g., network I/O) while it has computation budget left.
struct darwin_thread_time_constraint {
  std::chrono::microseconds period;
  std::chrono::microseconds computation;
  std::chrono::microseconds constraint;
  bool                       preemptible;
};

/// \brief Sets a Mach real-time time constraint on the calling thread. No-op off Darwin.
///
/// This is the closest equivalent of Linux SCHED_FIFO on macOS: it declares a recurring CPU deadline to the kernel,
/// so the thread is not preempted by network I/O or other background work within its computation window. The
/// constraint is attempted once per process; failures (e.g., unsupported kernels) are reported once and are not
/// fatal (the QoS class remains in effect). Note that the parameters are converted to Mach absolute time units
/// (mach_timebase_info) at runtime, so the microsecond values are timebase-independent.
void set_this_thread_time_constraint(const darwin_thread_time_constraint& constraint);

/// \brief Sets the Mach thread affinity tag of the calling thread (THREAD_AFFINITY_POLICY).
///
/// Threads sharing a non-zero tag are preferentially co-scheduled on the same L2 cluster. This emulates the Linux
/// CPU pinning: workers configured with the same CPU mask (i.e., the same pipeline) share a tag and thus share the
/// cluster, while workers configured for different cores get different tags and spread across clusters.
///
/// \note XNU implements THREAD_AFFINITY_POLICY on Intel only; on Apple Silicon (arm64) the policy is not supported
/// (thread_policy_set returns KERN_NOT_SUPPORTED) and this call is a no-op. On arm64 the QoS class is the supported
/// mechanism steering threads to the performance cores.
void set_this_thread_affinity_tag(int affinity_tag);

/// \brief Derives an affinity tag from a CPU mask.
///
/// - empty mask        -> 0 (no hint: unconfigured, scheduler is free to spread),
/// - 1 to 4 CPUs       -> lowest CPU index + 1 (pinning intent: strict or small-group co-location),
/// - wider than 4 CPUs -> 0 (a wide mask means "any of these cores" on Linux; there is no co-location intent).
int affinity_tag_from_cpu_mask(const os_sched_affinity_bitmask& mask);

/// \brief Fallback: derives a stable affinity tag from a thread/pool name.
///
/// Workers of the same pool are created as "<pool_name>#<idx>"; stripping the "#<idx>" suffix makes all workers of
/// a pool share one tag, while different pools (almost always) get different tags. This keeps L2 grouping meaningful
/// even when the configuration does not set explicit CPU masks.
int affinity_tag_from_thread_name(std::string_view name);

} // namespace ocudu
