// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// (Linux affinity branches relocated verbatim from upstream unique_thread.cpp.)

#include "ocudu/support/macos_compat.h"
#include "ocudu/ocudulog/ocudulog.h" // fetch_basic_logger (log_effective_decoder_backend)

#if defined(__APPLE__)
#include "ocudu/support/scheduling/darwin_thread_scheduling.h" // Darwin QoS / Mach time-constraint (kept in place, D5)
#include <libkern/OSByteOrder.h>                                // OSSwap* endian conversions
#include <mach/mach_time.h>                                     // mach_absolute_time(), mach_timebase_info()
#else
#include <cstring> // strerror()
#include <endian.h> // (undef'd below: the glibc le16toh-style macros must not collide with any compat name)
#include <sched.h> // sched_getcpu(), cpu_set_t, CPU_*(), pthread_setaffinity_np()
#include <time.h>  // clock_gettime(), CLOCK_MONOTONIC
#include "fmt/format.h"
#include "ocudu/support/cpu_architecture_info.h"
#undef le16toh
#undef htole16
#undef le32toh
#undef htole32
#endif
#include <cstdlib> // posix_memalign(), free()
#include <cstdio>  // std::sscanf / std::fprintf for the time-constraint arm parser and its report
#include <string>  // the time-constraint specification read from the environment
#include <mutex>
#include <unordered_map>
#include <thread>  // std::thread::hardware_concurrency()
#include <unistd.h> // sysconf(), _SC_PAGESIZE
#include <algorithm> // std::find()
#include <netinet/in.h> // sockaddr_in, sockaddr_in6
#include "ocudu/adt/span.h"

namespace ocudu {
namespace compat {

namespace {

/// Minimum alignment enforced by aligned_alloc(): one 64-byte cache line,
/// which covers the AVX-512 and NEON vector loads used by the PHY kernels.
constexpr size_t MIN_ALIGNMENT = 64;

#if defined(__APPLE__)
/// The Mach real-time time constraint the 2026-09-01 arm applied to EVERY real-time worker.
///
/// It is kept EXACTLY as it was, for one reason: it is the shape whose effect on a real radio leg is still
/// unknown, so the one-word arm that reproduces it must stay reachable.
///
/// \warning The comment that used to sit here claimed the constraint "keeps the thread on the performance
/// cores". The opposite is measured: applying a time constraint ERASES the thread's QoS class, and the QoS class
/// is the mechanism that steers a thread to the performance cores (measured 2026-10-01, dev doc 10.29).
///
/// What has since been measured about this exact shape (wip/sched_microbench/, 2x CPU oversubscription):
/// * it does NOT harm the thread that declares it - wakeup lateness stays at p50 4.9 / max 12.1 us, against
///   871 ms / 1.89 s for the same thread unconstrained, i.e. as good as a calibrated declaration;
/// * the 100% duty declaration does NOT starve other work: 16 spinner threads ran at 4.337 G-iter/s without it
///   and 4.445 G-iter/s with it;
/// * the only harmful shape is a malformed one (`constraint < computation`: p50 793 us / max 7.3 ms), which is
///   why apply_time_constraint_if_requested() refuses that shape instead of passing it to the kernel.
/// What it DOES do is give every worker the same deadline, flattening the inter-thread priority order that the
/// POSIX FIFO priorities (44/46/...) used to encode. That is the one candidate cause of the 2026-09-01
/// random-access regression no micro-benchmark can settle - hence opt-in, per-thread, and logged.
constexpr darwin_thread_time_constraint default_rt_time_constraint{
    std::chrono::microseconds{1000},
    std::chrono::microseconds{1000},
    std::chrono::microseconds{1000},
    true};

/// \brief Outcome of resolving OCUDU_SCHED_TIME_CONSTRAINT for one worker thread.
enum class tc_arm_outcome {
  /// The knob is off, or it names other threads: nothing is applied and nothing is printed.
  not_selected,
  /// This thread's arm was applied; the caller reports it.
  applied,
  /// The knob names this thread but the specification is malformed. NOTHING is applied, and this is reported:
  /// a leg that states an arm it did not get is worse than a leg that fails loudly (dev doc 10.29(5)).
  rejected
};

/// \brief Parses one "period/computation/constraint" value list (microseconds). False when malformed.
bool parse_time_constraint_values(std::string_view values, darwin_thread_time_constraint& tc)
{
  unsigned long period = 0;
  unsigned long comp   = 0;
  unsigned long cons   = 0;
  const std::string text(values);
  if (std::sscanf(text.c_str(), "%lu/%lu/%lu", &period, &comp, &cons) != 3) {
    return false;
  }
  if ((period == 0) || (comp == 0) || (cons == 0)) {
    return false;
  }
  tc = darwin_thread_time_constraint{std::chrono::microseconds{period},
                                     std::chrono::microseconds{comp},
                                     std::chrono::microseconds{cons},
                                     true};
  return true;
}

/// \brief True when the parameters are a shape this project has measured to be safe.
///
/// Only two rejections, both deliberate:
/// * `constraint < computation` - the one shape measured to be catastrophic (under load: p50 793 us, max 7.3 ms
///   of wakeup lateness, dev doc 10.29(2)). A deadline that expires before the work can be done is not a
///   conservative request, it is a malformed one;
/// * `period < constraint` - a deadline beyond its own period contradicts the declaration itself.
/// Everything else is accepted: sustained over-runs AND bursts inside the period were both measured harmless, so
/// refusing them would refuse arms that are known to work.
bool time_constraint_shape_is_sane(const darwin_thread_time_constraint& tc, std::string& why)
{
  if (tc.constraint < tc.computation) {
    why = fmt::format("constraint {}us < computation {}us (the one shape measured harmful: p50 793us / max 7.3ms)",
                      tc.constraint.count(),
                      tc.computation.count());
    return false;
  }
  if (tc.period < tc.constraint) {
    why = fmt::format(
        "period {}us < constraint {}us (a deadline beyond its own period)", tc.period.count(), tc.constraint.count());
    return false;
  }
  return true;
}

/// \brief Resolves the OCUDU_SCHED_TIME_CONSTRAINT arm for one worker thread and applies it if it selects one.
///
/// GRAMMAR (a leg must be able to state its arm in the environment, and the log must then prove it):
///   unset / "" / "0"                -> no constraint: the default, byte-identical behaviour
///   "1" / "default"                 -> the 2026-09-01 arm: EVERY worker gets default_rt_time_constraint
///   "NAME=P/C/K[;NAME=P/C/K...]"    -> per-thread microseconds; NAME="*" matches every worker. An exact NAME
///                                      match beats "*" whatever the order, so a default entry cannot silently
///                                      shadow a thread's own parameters.
///
/// WHERE IT APPLIES: on the worker thread itself, from apply_worker_thread_scheduling(), i.e. AFTER the QoS class
/// is requested - because that is the order that makes the consequence visible in the readback (the constraint
/// erases the class, so the thread it selects reads back UNSPECIFIED). It is NOT applied to io_timer/io_broker/
/// radio threads: this is a worker arm, and every thread this project has suspected of carrying the stall is a
/// worker.
tc_arm_outcome apply_time_constraint_if_requested(std::string_view               thread_name,
                                                  bool                           rt_intent,
                                                  darwin_thread_time_constraint& applied,
                                                  std::string&                   reject_reason)
{
  // Read on EVERY call rather than cached in a static, for the reason OCUDU_SCHED_ATTR_QOS gives: a cached
  // answer cannot be moved by a test. The cost is one getenv per worker thread creation.
  const char* env = std::getenv("OCUDU_SCHED_TIME_CONSTRAINT");
  if ((env == nullptr) || (env[0] == '\0') || ((env[0] == '0') && (env[1] == '\0'))) {
    return tc_arm_outcome::not_selected;
  }

  const std::string spec(env);
  if ((spec == "1") || (spec == "default")) {
    // The blanket arm reproduces 2026-09-01 in one word, including its scope: that arm applied the constraint to
    // the workers that declared a real-time intent, not to every thread in the process.
    if (!rt_intent) {
      return tc_arm_outcome::not_selected;
    }
    applied = default_rt_time_constraint;
    set_this_thread_time_constraint(applied);
    return tc_arm_outcome::applied;
  }

  // Two passes so an exact match wins over "*" regardless of where it appears in the string.
  for (int pass = 0; pass != 2; ++pass) {
    std::string_view rest = spec;
    while (!rest.empty()) {
      const size_t     sep   = rest.find(';');
      std::string_view entry = (sep == std::string_view::npos) ? rest : rest.substr(0, sep);
      rest                   = (sep == std::string_view::npos) ? std::string_view{} : rest.substr(sep + 1);
      if (entry.empty()) {
        continue;
      }
      const size_t eq = entry.find('=');
      if (eq == std::string_view::npos) {
        reject_reason = fmt::format("entry \"{}\" has no '=' (expected NAME=P/C/K)", entry);
        return tc_arm_outcome::rejected;
      }
      const std::string_view name  = entry.substr(0, eq);
      const bool             exact = (name == thread_name);
      const bool             any   = (name == "*");
      if (!(((pass == 0) && exact) || ((pass == 1) && any))) {
        continue;
      }
      darwin_thread_time_constraint tc{};
      if (!parse_time_constraint_values(entry.substr(eq + 1), tc)) {
        reject_reason = fmt::format("entry \"{}\" does not parse as NAME=P/C/K in microseconds", entry);
        return tc_arm_outcome::rejected;
      }
      if (!time_constraint_shape_is_sane(tc, reject_reason)) {
        return tc_arm_outcome::rejected;
      }
      applied = tc;
      set_this_thread_time_constraint(applied);
      return tc_arm_outcome::applied;
    }
  }
  return tc_arm_outcome::not_selected;
}

/// \brief Reports the time-constraint arm to stderr, on the thread that got it, with the values that were asked.
///
/// This line is NOT gated behind OCUDU_FLOW_PROBES, unlike the probe instruments: it is not a measurement, it is
/// the record of a scheduling change that has a regression history, and the 2026-09-01 incident is exactly what
/// an unprovable arm looks like. With the knob unset it prints nothing, so a default leg stays byte-identical.
void report_time_constraint_arm(std::string_view thread_name, const darwin_thread_time_constraint& tc)
{
  std::fprintf(stderr,
               "[sched_tc] thread=%.*s applied period=%lldus computation=%lldus constraint=%lldus duty=%.0f%% "
               "preemptible=%d | this thread has NO QoS class any more: on Darwin a Mach time constraint and a "
               "QoS class are mutually exclusive and the constraint wins (measured 2026-10-01, dev doc 10.29)\n",
               static_cast<int>(thread_name.size()),
               thread_name.data(),
               static_cast<long long>(tc.period.count()),
               static_cast<long long>(tc.computation.count()),
               static_cast<long long>(tc.constraint.count()),
               100.0 * static_cast<double>(tc.computation.count()) / static_cast<double>(tc.period.count()),
               tc.preemptible ? 1 : 0);
}
#endif

} // namespace

namespace {

/// Registry of the blocks handed out by aligned_alloc(), keyed by the block address.
///
/// The device mapping of a host buffer is only sound when the consumer knows which allocation a
/// pointer belongs to: the allocator hands the pages of a released block to the next one, so two
/// different buffers can occupy one page range over time. The registry keeps "base -> size" exact
/// (an entry is dropped by aligned_free()), which is all the wrap cache needs to keep the two apart.
struct aligned_registry {
  std::mutex                            mutex;
  std::unordered_map<const void*, size_t> blocks;
};

aligned_registry& registry()
{
  // Deliberately leaked: freed at exit, while other translation units may still describe pointers.
  static aligned_registry* r = new aligned_registry();
  return *r;
}

/// Observers of aligned_free() (see register_aligned_free_observer).
std::vector<aligned_free_observer>& free_observers()
{
  static std::vector<aligned_free_observer>* observers = new std::vector<aligned_free_observer>();
  return *observers;
}

} // namespace

void register_aligned_free_observer(aligned_free_observer observer)
{
  if (observer == nullptr) {
    return;
  }
  free_observers().push_back(observer);
}

void* aligned_alloc(size_t alignment, size_t size)
{
  if (alignment < MIN_ALIGNMENT) {
    alignment = MIN_ALIGNMENT;
  }

  // posix_memalign() requires a power-of-two alignment.
  if ((alignment & (alignment - 1)) != 0) {
    return nullptr;
  }

  // The public contract accepts any size; round it up so the returned block
  // always covers the full requested byte range.
  const size_t rounded_size = (size + alignment - 1) & ~(alignment - 1);

  void* ptr = nullptr;
  if (::posix_memalign(&ptr, alignment, rounded_size) != 0) {
    return nullptr;
  }

  aligned_registry& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  r.blocks[ptr] = rounded_size;
  return ptr;
}

void aligned_free(void* ptr)
{
  if (ptr == nullptr) {
    return;
  }
  {
    aligned_registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    r.blocks.erase(ptr);
  }
  // The registry no longer describes the block, so the observers run outside the registry lock: a
  // consumer that keeps state per allocation (the Metal wrap cache) takes its own lock, and nesting
  // the two in the opposite order to describe_aligned_allocation() would deadlock. They run before
  // the block is released, so whatever they drop cannot be overtaken by the next allocation that
  // gets these pages.
  for (aligned_free_observer observer : free_observers()) {
    observer(ptr);
  }
  ::free(ptr);
}

bool describe_aligned_allocation(const void* ptr, void** base, size_t* size)
{
  if (ptr == nullptr) {
    return false;
  }
  const char* p = static_cast<const char*>(ptr);
  aligned_registry& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  // The block that contains the pointer: the highest base at or below it.
  const void* best      = nullptr;
  size_t      best_size = 0;
  for (const auto& entry : r.blocks) {
    const char* b = static_cast<const char*>(entry.first);
    if ((p >= b) && (static_cast<size_t>(p - b) < entry.second)) {
      if ((best == nullptr) || (b > static_cast<const char*>(best))) {
        best      = entry.first;
        best_size = entry.second;
      }
    }
  }
  if (best == nullptr) {
    return false;
  }
  if (base != nullptr) {
    *base = const_cast<void*>(best);
  }
  if (size != nullptr) {
    *size = best_size;
  }
  return true;
}

size_t page_size()
{
  static const size_t value = []() {
    long sz = ::sysconf(_SC_PAGESIZE);
    return sz > 0 ? static_cast<size_t>(sz) : static_cast<size_t>(4096);
  }();
  return value;
}

uint64_t get_monotonic_time_us()
{
#if defined(__APPLE__)
  // Mach absolute time is monotonic and unaffected by wall-clock changes. The
  // timebase conversion factor is constant for the lifetime of the process.
  static const mach_timebase_info_data_t timebase = []() {
    mach_timebase_info_data_t tb = {};
    ::mach_timebase_info(&tb);
    return tb;
  }();

  const uint64_t ticks = ::mach_absolute_time();
  const uint64_t nanos = ticks * timebase.numer / timebase.denom;
  return nanos / 1000;
#else
  struct timespec ts;
  if (::clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0;
  }
  return static_cast<uint64_t>(ts.tv_sec) * 1000000U + static_cast<uint64_t>(ts.tv_nsec) / 1000U;
#endif
}

unsigned get_current_cpu()
{
#if defined(__APPLE__)
  // macOS exposes no cheap per-thread CPU index to user space (sched_getcpu()
  // is Linux-only). Keep the exact behaviour previously inlined at the call
  // sites: the CPU-aware distribution degenerates to a fixed offset.
  return 0;
#else
  const int cpu = ::sched_getcpu();
  return cpu >= 0 ? static_cast<unsigned>(cpu) : 0U;
#endif
}

std::vector<size_t> get_available_cpu_ids()
{
#if defined(__APPLE__)
  // Apple Silicon maps physical cores to logical CPUs 1:1; the process may
  // run on all of them.
  const unsigned      nof_cpus = std::thread::hardware_concurrency();
  std::vector<size_t> ids(nof_cpus);
  for (unsigned i = 0; i != nof_cpus; ++i) {
    ids[i] = i;
  }
  return ids;
#else
  std::vector<size_t> ids;
  const ::cpu_set_t   cpuset = cpu_architecture_info::get().get_available_cpuset();
  for (unsigned i = 0; i != CPU_SETSIZE; ++i) {
    if (CPU_ISSET(i, &cpuset)) {
      ids.push_back(i);
    }
  }
  return ids;
#endif
}

// 4. Thread & core scheduling.

void set_thread_realtime_priority()
{
#if defined(__APPLE__)
  // The QoS class is the supported mechanism steering latency-critical threads
  // to the performance cores on Apple Silicon.
  set_this_thread_qos_class(QOS_CLASS_USER_INTERACTIVE);
#endif
}

void bind_thread_to_performance_core()
{
#if defined(__APPLE__)
  set_thread_realtime_priority();
  set_this_thread_time_constraint(default_rt_time_constraint);
#endif
}

void configure_worker_thread_attributes(::pthread_attr_t& attr)
{
#if defined(__APPLE__)
  // The macOS default pthread stack (512 KiB) is too small for the gNB's deep
  // call chains: the FAPI fastpath translators keep large TTI structures on
  // the stack, which overflow the default stack. Use a stack size similar to
  // the Linux default (8 MiB), with margin.
  ::pthread_attr_setstacksize(&attr, 16u * 1024u * 1024u);
#endif
}

void configure_worker_thread_attributes_qos(::pthread_attr_t& attr, const os_thread_realtime_priority& prio)
{
#if defined(__APPLE__)
  // Off by default, and gated by the environment rather than by a build option so the two arms can be compared
  // on ONE binary: the A/B this switch exists for is "does the worker start on a P-core", and rebuilding
  // between arms would put the build itself into the comparison.
  //
  // The variable is read ON EVERY CALL rather than cached in a static, for the reason the probes' own knobs give
  // (see ul_pipeline_probe::stale_after_us): a cached answer cannot be moved by a test, and a switch nobody can
  // exercise in a unit test is a switch nobody can trust. The cost is one getenv per thread creation.
  const char* env = std::getenv("OCUDU_SCHED_ATTR_QOS");
  if ((env == nullptr) || (env[0] == '\0') || ((env[0] == '0') && (env[1] == '\0'))) {
    return;
  }
  set_pthread_attr_qos_class(attr, darwin_qos_class_for_prio(prio));
#else
  // Linux: the POSIX priority applied by the thread wrapper is the native mechanism and is enforced there, so
  // there is nothing to declare on the attributes - and this arm must not even look at the environment, so a
  // Linux run is byte-identical whether the variable is set or not.
  (void)attr;
  (void)prio;
#endif
}

bool set_thread_name(::pthread_t thread, const char* name)
{
#if defined(__APPLE__)
  (void)thread; // macOS pthread_setname_np() names the calling thread.
  return ::pthread_setname_np(name) == 0;
#else
  return ::pthread_setname_np(thread, name) == 0;
#endif
}

void apply_worker_thread_scheduling(const os_thread_realtime_priority& prio,
                                    const os_sched_affinity_bitmask&  cpu_mask,
                                    std::string_view                  thread_name)
{
#if defined(__APPLE__)
  // Darwin scheduling: POSIX SCHED_FIFO and CPU pinning are not enforceable on
  // macOS. Instead, elevate the QoS class and set the Mach affinity tag of the
  // thread:
  // - QoS: real-time intent -> QOS_CLASS_USER_INTERACTIVE, otherwise
  //   QOS_CLASS_USER_INITIATED (both keep the thread on the performance
  //   cores);
  // - affinity tag: derived from the configured CPU mask when present,
  //   otherwise from the worker pool name, so threads sharing a pipeline are
  //   co-located on one L2 cluster.
  //
  // NOTE (2026-09-01): the automatic Mach time-constraint application tried here earlier
  // (bind_thread_to_performance_core) changed the runtime scheduling semantics versus the
  // historical port and coincided with an OAI-UE random-access regression under
  // ENABLE_FLOW_PROBES. The historical behaviour (QoS + tag only, with the POSIX
  // setschedparam still attempted by the caller) is restored; bind_thread_to_performance_core()
  // remains available as an explicit opt-in.
  set_this_thread_qos_class(darwin_qos_class_for_prio(prio));
  set_this_thread_affinity_tag(cpu_mask.any() ? affinity_tag_from_cpu_mask(cpu_mask)
                                              : affinity_tag_from_thread_name(thread_name));

  // ★ P4 (dev doc 10.29): the Mach time constraint - the only macOS mechanism that reserves CPU, measured at
  // 700x on the wakeup tail under 2x oversubscription (5358 us -> 9.9 us) - applied HERE, after the QoS class,
  // and only when the environment names this thread. Two consequences are deliberate:
  // * the constraint ERASES the class just set above (they are mutually exclusive on Darwin, and the class
  //   cannot be restored afterwards: set_qos_class_self_np then returns EPERM). That is why the arm is reported
  //   with the sentence it is reported with, and why the readback prints `tc=` next to `eff=`;
  // * it is opt-in and per-thread, because the 2026-09-01 arm's one remaining suspect is not the constraint but
  //   giving every worker the SAME parameters, which flattens the inter-thread priority order.
  {
    darwin_thread_time_constraint tc{};
    std::string                   reject_reason;
    switch (apply_time_constraint_if_requested(
        thread_name, prio != os_thread_realtime_priority::no_realtime(), tc, reject_reason)) {
      case tc_arm_outcome::applied:
        report_time_constraint_arm(thread_name, tc);
        break;
      case tc_arm_outcome::rejected:
        std::fprintf(stderr,
                     "[sched_tc] thread=%.*s REJECTED, nothing applied: %s\n",
                     static_cast<int>(thread_name.size()),
                     thread_name.data(),
                     reject_reason.c_str());
        break;
      case tc_arm_outcome::not_selected:
        break;
    }
  }
#else
  (void)prio;
  (void)cpu_mask;
  (void)thread_name;
#endif
}

void print_thread_affinity_info(::pthread_t thread)
{
#if defined(__APPLE__)
  (void)thread;
  fmt::println("Thread affinity query is not supported on macOS.");
#else
  ::cpu_set_t cpuset;

  int s = ::pthread_getaffinity_np(thread, sizeof(::cpu_set_t), &cpuset);
  if (s != 0) {
    fmt::println("error pthread_getaffinity_np: {}", ::strerror(s));
  }

  fmt::println("Set returned by pthread_getaffinity_np() contained:");
  for (unsigned j = 0; j != CPU_SETSIZE; ++j) {
    if (CPU_ISSET(j, &cpuset)) {
      fmt::println("    CPU {}", j);
    }
  }
#endif
}

bool set_thread_affinity(::pthread_t                      thread,
                         const os_sched_affinity_bitmask& cpu_mask,
                         const std::string&               thread_name)
{
#if defined(__APPLE__)
  // XNU exposes no user-space CPU pinning; the QoS class and affinity tag
  // applied in apply_worker_thread_scheduling() are the scheduling mechanisms.
  (void)thread;
  (void)cpu_mask;
  (void)thread_name;
  return true;
#else
  // Warn about CPU IDs in the mask that are not available to this process
  // (same set as os_sched_affinity_bitmask::available_cpus(), computed locally
  // so this module does not depend back on ocudu_support).
  const auto available_ids = get_available_cpu_ids();
  std::vector<size_t> invalid_ids;
  for (size_t i = 0, e = cpu_mask.size(); i != e; ++i) {
    if (cpu_mask.test(i) && std::find(available_ids.begin(), available_ids.end(), i) == available_ids.end()) {
      invalid_ids.push_back(i);
    }
  }
  if (!invalid_ids.empty()) {
    fmt::println("Warning: The CPU affinity of thread \"{}\" contains the following invalid CPU ids: {}",
                 thread_name,
                 span<const size_t>(invalid_ids));
  }

  ::cpu_set_t* cpusetp     = CPU_ALLOC(cpu_mask.size());
  size_t       cpuset_size = CPU_ALLOC_SIZE(cpu_mask.size());
  CPU_ZERO_S(cpuset_size, cpusetp);

  for (size_t i = 0, e = cpu_mask.size(); i != e; ++i) {
    if (cpu_mask.test(i)) {
      CPU_SET_S(i, cpuset_size, cpusetp);
    }
  }

  int ret;
  if ((ret = ::pthread_setaffinity_np(thread, cpuset_size, cpusetp)) != 0) {
    fmt::print("Couldn't set affinity for {} thread. Cause: '{}'\n", thread_name, ::strerror(ret));
    CPU_FREE(cpusetp);
    return false;
  }

  CPU_FREE(cpusetp);
  return true;
#endif
}

bool posix_realtime_priority_is_enforceable()
{
#if defined(__APPLE__)
  // ★★ MEASURED 2026-10-01, AND IT REVERSES WHAT THIS FUNCTION USED TO SAY (dev doc 10.5). The claim used to be
  // "pthread_setschedparam(SCHED_FIFO) succeeds without privileges and is recorded by the kernel, while the QoS
  // class remains the effective scheduling mechanism". The second half is FALSE, and the first half is what makes
  // it false: on Darwin a thread is EITHER QoS-managed OR explicitly scheduled, never both, and the POSIX call
  // silently converts it to the latter -
  //
  //   set_qos_class_self_np(USER_INTERACTIVE)          -> 0, readback USER_INTERACTIVE
  //   pthread_setschedparam(SCHED_FIFO, 46)            -> 0, readback UNSPECIFIED   <- the class is GONE
  //   set_qos_class_self_np(USER_INTERACTIVE)  (again) -> 1 (EPERM), class stays UNSPECIFIED - for the lifetime
  //                                                       of the thread, even after switching back to SCHED_OTHER
  //
  // and the same happens to a class declared on the ATTRIBUTES at creation (so P3's attr-QoS cannot help while
  // this call remains). The end-to-end consequence was measured on a loopback run with OCUDU_SCHED_VERBOSE=1: a
  // real-time worker printed `req=USER_INTERACTIVE eff=UNSPECIFIED posix=FIFO/44`, while a NON-real-time worker
  // (which never calls this function) printed `req=USER_INITIATED eff=USER_INITIATED`. That is, the data-plane
  // threads have been running with NO QoS class at all - below the io_timer/io_broker threads they are supposed
  // to outrank - since the day the POSIX call was added, and nothing could see it because nothing read it back.
  //
  // ★★★ THE DEFAULT IS NOW "DO NOT APPLY IT" (user ruling, 2026-10-01, same evening as the measurement).
  //
  // The two arms are not symmetric and the ruling follows the evidence: with the POSIX call the data-plane
  // threads have NO QoS class (measured on the bench AND on three radio legs), and with it skipped they keep
  // USER_INTERACTIVE. What is given up is a SCHED_FIFO policy that this port has never shown to be enforced on
  // macOS - its only demonstrated effect was to erase the class. So the default now protects the mechanism that
  // demonstrably works, and the OLD behaviour stays reachable as a measurement arm:
  //
  //   (default)                     -> the POSIX parameters are NOT applied; the QoS class survives
  //   OCUDU_SCHED_POSIX_RT=1        -> the historical arm: apply them, and lose the class (the A/B's control)
  //
  // It stays an ENVIRONMENT switch rather than a build option so the two arms are comparable on ONE binary - a
  // rebuild between arms would put the build itself into the comparison.
  //
  // \note The switch was named OCUDU_SCHED_SKIP_POSIX_RT while it was an opt-in; it never flew a leg, so the
  //       name changed with the default instead of accumulating a second, inverted knob.
  const char* env = std::getenv("OCUDU_SCHED_POSIX_RT");
  if (env == nullptr) {
    return false;
  }
  return !((env[0] == '\0') || ((env[0] == '0') && (env[1] == '\0')));
#else
  // Linux: SCHED_FIFO IS the mechanism there and it is enforced, so it is always applied.
  return true;
#endif
}

os_thread_realtime_priority radio_worker_realtime_priority()
{
#if defined(__APPLE__)
  // On macOS, the radio channel loop moves the RF samples in/out of the
  // baseband (ZMQ or OFH): treat it as real-time so that it is elevated to
  // QOS_CLASS_USER_INTERACTIVE.
  return os_thread_realtime_priority::max() - 1;
#else
  return os_thread_realtime_priority::no_realtime();
#endif
}

} // namespace compat

// Affinity-tag derivation helpers. Declared by darwin_thread_scheduling.h
// (kept in place per the compat-layer aggregation design); the definitions
// live here, in the compat target, so both the compat wrappers and the Darwin
// module share one implementation without a link cycle with ocudu_support.

int affinity_tag_from_cpu_mask(const os_sched_affinity_bitmask& mask)
{
  if (not mask.any() || mask.count() > 4) {
    return 0;
  }
  return mask.find_lowest(0, mask.size()) + 1;
}

int affinity_tag_from_thread_name(std::string_view name)
{
  // Strip the worker index suffix: "main_pool#2" -> "main_pool".
  std::string_view pool_prefix = name;
  if (auto pos = pool_prefix.rfind('#'); pos != std::string_view::npos) {
    pool_prefix = pool_prefix.substr(0, pos);
  }

  // FNV-1a, folded into [1, 4095] (0 is reserved for "no hint"). With ~10
  // distinct pools, collisions are negligible.
  uint32_t hash = 2166136261u;
  for (char c : pool_prefix) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 16777619u;
  }
  return 1 + static_cast<int>(hash % 4095);
}

namespace compat {

// 5. Networking (UDP) compat.

int sendmmsg(int sockfd, mmsghdr* msgvec, unsigned vlen, int flags)
{
#if defined(__APPLE__)
  // macOS has no sendmmsg(): send every message with ::sendmsg.
  for (unsigned i = 0; i < vlen; ++i) {
    ssize_t res = ::sendmsg(sockfd, &msgvec[i].msg_hdr, flags);
    if (res < 0) {
      return i > 0 ? static_cast<int>(i) : -1;
    }
    msgvec[i].msg_len = static_cast<unsigned>(res);
  }
  return static_cast<int>(vlen);
#else
  static_assert(sizeof(mmsghdr) == sizeof(::mmsghdr));
  return ::sendmmsg(sockfd, reinterpret_cast<::mmsghdr*>(msgvec), vlen, flags);
#endif
}

int recvmmsg(int sockfd, mmsghdr* msgvec, unsigned vlen, int flags, void* timeout)
{
#if defined(__APPLE__)
  (void)timeout;
  // macOS has no recvmmsg(): emulate the Linux MSG_WAITFORONE semantics the caller relies on - wait for at least
  // one datagram, then return everything already buffered (up to vlen). Block on the first recvmsg, then drain
  // with MSG_DONTWAIT until EAGAIN. Keeping the callback short is essential: the io_broker re-arms the fd after
  // every callback and the level-triggered EVFILT_READ fires again while data is pending, so a busy socket is
  // drained by successive short callbacks. The previous emulation looped blocking recvmsg calls up to vlen times,
  // so a slow trickle of datagrams held the callback for one inter-packet gap per packet - with vlen=256 and a
  // 10 pps flow the receive path stalled for tens of seconds and delivered the E2E ping replies in ~26 s bursts.
  unsigned i   = 0;
  ssize_t  res = ::recvmsg(sockfd, &msgvec[0].msg_hdr, flags & ~MSG_DONTWAIT);
  if (res < 0) {
    return -1;
  }
  msgvec[0].msg_len = static_cast<unsigned>(res);
  i                 = 1;
  for (; i < vlen; ++i) {
    res = ::recvmsg(sockfd, &msgvec[i].msg_hdr, flags | MSG_DONTWAIT);
    if (res < 0) {
      // Nothing left to read (EAGAIN/EWOULDBLOCK) or a real error: report the datagrams received so far.
      break;
    }
    msgvec[i].msg_len = static_cast<unsigned>(res);
  }
  return static_cast<int>(i);
#else
  static_assert(sizeof(mmsghdr) == sizeof(::mmsghdr));
  return ::recvmmsg(sockfd, reinterpret_cast<::mmsghdr*>(msgvec), vlen, flags, static_cast<::timespec*>(timeout));
#endif
}

socklen_t sockaddr_length_for_send(const sockaddr_storage& addr)
{
#if defined(__APPLE__)
  // macOS rejects sendmsg() with the full sockaddr_storage size as msg_namelen for IPv6 destinations (EINVAL);
  // only the exact family-specific length is accepted.
  switch (addr.ss_family) {
    case AF_INET:
      return sizeof(sockaddr_in);
    case AF_INET6:
      return sizeof(sockaddr_in6);
    default:
      return sizeof(sockaddr_storage);
  }
#else
  (void)addr;
  return sizeof(sockaddr_storage);
#endif
}

// 6. Lower PHY / data-plane helpers.

void lower_phy_stop_chain_end(std::promise<void>& stop_control)
{
#if defined(__APPLE__)
  // The stop completes when the processing task actually finishes (see lower_phy_stop_task_end()).
  (void)stop_control;
#else
  stop_control.set_value();
#endif
}

void lower_phy_stop_task_end(uint32_t state, bool wait_stop, uint32_t state_stopped, std::promise<void>& stop_control)
{
#if defined(__APPLE__)
  if (wait_stop && state >= state_stopped) {
    stop_control.set_value();
  }
#else
  (void)state;
  (void)wait_stop;
  (void)state_stopped;
  (void)stop_control;
#endif
}

bool drain_executor_on_stop(task_executor& executor)
{
#if defined(__APPLE__)
  // The FSM counters only track the self-deferred processing chains, while tasks deferred right before the stop
  // was requested are not covered by them. Deferring a sentinel task and waiting for its completion guarantees
  // that every previously enqueued task has finished when this returns.
  std::promise<void> flush;
  if (not executor.defer([&flush]() { flush.set_value(); })) {
    return false;
  }
  flush.get_future().wait();
#else
  (void)executor;
#endif
  return true;
}

void wait_for_tx_timestamp()
{
#if defined(__APPLE__)
  // Do not use sleep_for here: macOS coalesces short sleeps under load, so a 100 us request can actually sleep
  // several milliseconds and overshoot the 2 ms wall-clock deadline by a large margin. Spin with the YIELD hint
  // instead: the exit precision is exact and the spin is bounded by the 2 ms deadline.
  cpu_relax();
#else
  std::this_thread::sleep_for(std::chrono::microseconds(10));
#endif
}

bool poll_rx_wait_enabled()
{
#if defined(__APPLE__)
  // Two keys, like every instrument here: the compile switch is the caller's guard, this is the run-time one.
  const char* env = std::getenv("OCUDU_UL_RX_POLL_WAIT");
  return (env != nullptr) && (env[0] != '\0') && !((env[0] == '0') && (env[1] == '\0'));
#else
  // Linux never reaches the call site (it is inside a platform guard), and saying "false" here keeps that
  // guarantee checkable rather than assumed.
  return false;
#endif
}

// ---------------------------------------------------------------------------------------------------
// P6.1: the LANE GRID (see macos_compat.h for what it is and why a delay cannot do its job).
// ---------------------------------------------------------------------------------------------------
namespace {

/// Slot counter wrap: slot_point::count() is a hyperframe-relative counter, so a distance between two slots
/// has to be taken modulo the hyperframe. It is a parameter because the caller knows the numerology.
constexpr uint64_t kSlotsPerHyperframe = 1024 * 20; // 30 kHz SCS; the caller passes its own through the API

#if defined(__APPLE__)
// The STATE and the helpers below are macOS-only, and the guard is here rather than around the whole block for a
// reason Linux found immediately: `-Werror=unused-function` fires on env_us/steady_now_ns when the call sites
// compile to no-ops, which is exactly the kind of "the Linux build is a different build" this port must not have.
// The PURE functions (target mapping, grid update) stay outside the guard because they are the tested arithmetic
// and the unit test runs on both platforms.
/// The delivery-lag histogram's shape: a SIGNED fixed-resolution range, because a lag can be NEGATIVE (the
/// frontier can arrive before the grid instant) and a log2 bucket built with a shift keeps the sign bit set for
/// every negative value - so all of them would land in the top bucket and every percentile would read as that
/// bucket's edge. Measured: the first version printed p50 = 16777215 us against a min/max of -44/+11.
constexpr int64_t  LAG_LO_US     = -32768;
constexpr int64_t  LAG_RES_US    = 64;
constexpr unsigned LAG_BUCKETS   = 1024; // 64 ms of range at 64 us resolution

struct lane_grid_state {
  std::atomic<int64_t>  anchor_host_ns{0};
  std::atomic<int64_t>  anchor_slot{-1};
  std::atomic<int64_t>  slot_duration_ns{500000};
  std::atomic<int64_t>  lead_ns{200000};
  std::atomic<uint64_t> noted{0};
  /// The last slot index seen and its UNWRAPPED distance from the anchor - the pair the distance accumulation
  /// needs (see lane_grid_unwrap_distance). Guarded by `distance_mutex`, which is uncontended: two callers, one
  /// per slot at most.
  std::mutex            distance_mutex;
  uint64_t              last_slot     = 0;
  int64_t               last_distance = 0;
  std::atomic<uint64_t> clamped{0};
  std::atomic<uint64_t> late{0};
  std::atomic<uint64_t> unarmed{0};
  std::atomic<uint64_t> rearmed{0};
  std::atomic<int64_t>  wait_sum_us{0};
  std::atomic<int64_t>  wait_max_us{0};
  /// How far PAST its target the clamp actually returned, per hop: the clamp's own precision.
  ///
  /// It separates two things the wait distribution cannot: a clamp that is precise (a few microseconds - the
  /// spin's resolution) from a THREAD THAT WAS TAKEN OFF THE CPU during its own clamp. The second is what the
  /// time-constraint arm exists for, and this is the reading that says whether there is anything for it to fix
  /// at THIS scale - the 1 ms watchdog cannot see it (see the lane grid's header note).
  std::atomic<int64_t>  overshoot_sum_us{0};
  std::atomic<int64_t>  overshoot_max_us{0};
  /// \name The FRONTIER's delivery lag: host instant the samples were readable MINUS the grid instant they
  /// were due at, i.e. how long the radio-to-host path took. This is the distribution `lead` has to cover, and
  /// it is the reading the absolute time base exists for.
  ///@{
  std::atomic<uint64_t> lag_n{0};
  std::atomic<int64_t>  lag_min_us{0};
  std::atomic<int64_t>  lag_max_us{0};
  /// Signed fixed-resolution histogram (see LAG_* above), so the report prints percentiles from bounded state.
  std::mutex            lag_mutex;
  uint64_t              lag_buckets[LAG_BUCKETS] = {};
  ///@}
  /// Whether a radio ABSOLUTE time has ever been seen (see metadata::absolute_ns), and the pair it anchored.
  std::atomic<bool>     absolute_seen{false};
  std::atomic<int64_t>  absolute_anchor_ns{-1};
  /// The first and latest (radio absolute, host) pairs, for the host-vs-radio RATE - the drift check the
  /// absolute time base exists for. Both clocks are crystals, so the interesting number is ppm, and it is only
  /// visible over a baseline: 100 us of lag jitter over 100 s of baseline is 1 ppm.
  std::atomic<int64_t>  abs_first_ns{0};
  std::atomic<int64_t>  abs_last_ns{0};
  std::atomic<int64_t>  host_first_ns{0};
  std::atomic<int64_t>  host_last_ns{0};
  std::atomic<bool>     params_read{false};
};

/// The filter's gain as a right shift (see the caller): read once from the environment, default 1/1024.
int64_t st_gain_shift()
{
  static const int64_t shift = []() {
    const char* env = std::getenv("OCUDU_UL_LANE_GRID_GAIN_SHIFT");
    if ((env == nullptr) || (env[0] == '\0')) {
      // 1/65536, and the default is the DESIGN, not a tune: the grid must be a CLOCK over the arrivals, not a
      // tracker of them. `late` is only a verdict, and the lag below is only a measurement, if the grid refuses
      // to absorb what it is measuring - at 1/64 a 100 us lag excursion moved the grid 1.5 us and at 1/1024 it
      // moved 0.1 us, which is enough to hide a lead that has stopped being sufficient. At 1/65536 the same
      // excursion moves it 1.5 ns, while a 10 ppm host-vs-radio drift is still tracked in ~30 s (65536
      // observations at the measured ~2000 slot-frontiers per second).
      return static_cast<int64_t>(16);
    }
    const long v = std::strtol(env, nullptr, 10);
    // 6 = 1/64 (the first default) is the fastest sensible; 16 keeps the arithmetic in range and is slower than
    // any drift needs. Refuse anything else rather than accept a gain that makes the filter a follower or a
    // constant.
    return (v < 6 || v > 20) ? static_cast<int64_t>(16) : static_cast<int64_t>(v);
  }();
  return shift;
}

lane_grid_state& lane_grid()
{
  // Never destroyed, for the reason rx_pool_accounts() spells out: the report is an atexit handler.
  static lane_grid_state* st = new lane_grid_state();
  return *st;
}



int64_t env_us(const char* name, int64_t fallback)
{
  const char* env = std::getenv(name);
  if ((env == nullptr) || (env[0] == '\0')) {
    return fallback;
  }
  const long v = std::strtol(env, nullptr, 10);
  return (v < 0) ? fallback : static_cast<int64_t>(v) * 1000;
}

int64_t steady_now_ns()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void lane_grid_report_impl()
{
  lane_grid_state& st = lane_grid();
  if ((st.noted.load(std::memory_order_relaxed) == 0) && (st.unarmed.load(std::memory_order_relaxed) == 0)) {
    return; // the knob was off (or the grid never saw a slot): print nothing rather than a line of zeroes
  }
  const uint64_t clamped = st.clamped.load(std::memory_order_relaxed);
  const double   mean_us =
      (clamped != 0) ? (static_cast<double>(st.wait_sum_us.load(std::memory_order_relaxed)) /
                        static_cast<double>(clamped))
                     : 0.0;
  std::fprintf(stderr,
               "[lane_grid] OCUDU_UL_LANE_GRID=1: grid armed from %llu hop(s), re-armed %llu time(s); "
               "clamped=%llu (mean wait %.1fus, max %lldus; overshoot mean %.1fus, max %lldus); late=%llu; "
               "unarmed=%llu. late counts the hops whose grid instant had ALREADY passed when the lane began - "
               "it is the lead's verdict, and it is the number to read first; overshoot is how far past its own "
               "target the clamp returned, i.e. whether the thread was descheduled inside its clamp\n",
               static_cast<unsigned long long>(st.noted.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(st.rearmed.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(clamped),
               mean_us,
               static_cast<long long>(st.wait_max_us.load(std::memory_order_relaxed)),
               (clamped != 0) ? (static_cast<double>(st.overshoot_sum_us.load(std::memory_order_relaxed)) /
                                 static_cast<double>(clamped))
                              : 0.0,
               static_cast<long long>(st.overshoot_max_us.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(st.late.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(st.unarmed.load(std::memory_order_relaxed)));
  if (st.lag_n.load(std::memory_order_relaxed) != 0) {
    std::lock_guard<std::mutex> lock(st.lag_mutex);
    // The bucket's CENTRE, so a percentile never claims more resolution than the histogram has.
    const auto pct = [&st](double q) {
      const uint64_t target = static_cast<uint64_t>(q * static_cast<double>(st.lag_n.load()) + 0.999999);
      uint64_t       seen   = 0;
      for (unsigned i = 0; i != LAG_BUCKETS; ++i) {
        seen += st.lag_buckets[i];
        if (seen >= target) {
          return LAG_LO_US + static_cast<int64_t>(i) * LAG_RES_US + LAG_RES_US / 2;
        }
      }
      return st.lag_max_us.load();
    };
    std::fprintf(stderr,
                 "[lane_grid]   FRONTIER delivery lag (radio -> host readable, us): n=%llu p50=%lld p95=%lld "
                 "p99=%lld max=%lld min=%lld; radio absolute time %s. This is the distribution a periodic "
                 "commit thread's `lead` has to cover\n",
                 static_cast<unsigned long long>(st.lag_n.load(std::memory_order_relaxed)),
                 static_cast<long long>(pct(0.50)),
                 static_cast<long long>(pct(0.95)),
                 static_cast<long long>(pct(0.99)),
                 static_cast<long long>(st.lag_max_us.load(std::memory_order_relaxed)),
                 static_cast<long long>(st.lag_min_us.load(std::memory_order_relaxed)),
                 st.absolute_seen.load(std::memory_order_relaxed) ? "REPORTED" : "not reported (-1): lag is relative");
    // The host-vs-radio RATE, when both ends of a baseline exist: the drift check, and the only thing the
    // absolute epoch is needed for once the grid is anchored (see the gain's note).
    const int64_t abs_span  = st.abs_last_ns.load(std::memory_order_relaxed) - st.abs_first_ns.load(std::memory_order_relaxed);
    const int64_t host_span = st.host_last_ns.load(std::memory_order_relaxed) - st.host_first_ns.load(std::memory_order_relaxed);
    if ((abs_span > 1000000000LL) && (host_span > 0)) {
      const double rate_ppm = (static_cast<double>(host_span) / static_cast<double>(abs_span) - 1.0) * 1e6;
      std::fprintf(stderr,
                   "[lane_grid]   host-vs-radio rate over %.1fs of baseline: %.3f ppm, applied to the "
                   "extrapolation (the offset is NOT walked: see the grid's header)\n",
                   static_cast<double>(abs_span) / 1e9,
                   rate_ppm);
    }
  }
}

const bool lane_grid_report_registered = []() {
  std::atexit(lane_grid_report_impl);
  // NOTE: the on-demand (P0) registry is NOT wired here on purpose. It lives in the PHY layer, and this file is
  // a util that must not depend on it - found when the compat unit test failed to LINK on
  // ocudu::register_p0_report. The layer that USES the grid registers lane_grid_report() next to its own
  // reports instead (lower_phy_baseband_processor.cpp, beside rx_pool_report), which is also where the reason
  // belongs: the gNB's cleanup path dumps the P0 readings and then raises SIGKILL, so an atexit-only report is
  // lost whenever the stop takes the forced-exit branch.
  return true;
}();

#endif // __APPLE__

} // namespace

bool lane_grid_enabled()
{
#if defined(__APPLE__)
  // The usual two keys: the call sites are also inside a platform guard, so Linux cannot reach this at all.
  const char* env = std::getenv("OCUDU_UL_LANE_GRID");
  if ((env == nullptr) || (env[0] == '\0') || ((env[0] == '0') && (env[1] == '\0'))) {
    return false;
  }
  lane_grid_state& st = lane_grid();
  if (!st.params_read.exchange(true, std::memory_order_relaxed)) {
    st.lead_ns.store(env_us("OCUDU_UL_LANE_GRID_LEAD_US", 200), std::memory_order_relaxed);
  }
  return true;
#else
  return false;
#endif
}

/// \brief The extrapolation's host-vs-radio rate correction, in parts per billion (0 = identical rates).
///
/// It is the ONLY quantity that moves the grid once the offset is anchored - see lane_grid_update_ns() for why
/// the offset must not move, and lane_grid_note_slot() for how this is estimated.
///
/// IT LIVES OUTSIDE THE PLATFORM GUARD ON PURPOSE, and the Linux build is what said so: the target mapping and
/// its test hook are compiled on BOTH platforms (the arithmetic is platform-neutral and the unit test runs on
/// Linux too), so the value they read has to exist on both. The ESTIMATOR of it is macOS-only, with the grid.
std::atomic<int64_t> g_lane_grid_rate_ppb{0};

int64_t lane_grid_rate_ppb()
{
  return g_lane_grid_rate_ppb.load(std::memory_order_relaxed);
}

int64_t lane_grid_target_ns(int64_t anchor_host_ns, int64_t distance_slots, int64_t slot_duration_ns, int64_t lead_ns)
{
  if ((distance_slots < 0) || (slot_duration_ns <= 0)) {
    return -1;
  }
  const int64_t radio_span_ns = distance_slots * slot_duration_ns;
  // `rate_ppb` is the host-vs-radio rate correction (see lane_grid_rate_ppb): the ONLY thing allowed to move
  // the extrapolation, because it is the only thing that is a property of the two CLOCKS rather than of the
  // transport. 0 means "same rate", which is what a software loopback radio really is.
  return anchor_host_ns + radio_span_ns +
         static_cast<int64_t>((static_cast<double>(radio_span_ns) * static_cast<double>(lane_grid_rate_ppb())) / 1e9) +
         lead_ns;
}

int64_t lane_grid_unwrap_distance(uint64_t prev_slot,
                                  int64_t  prev_distance,
                                  uint64_t slot,
                                  uint64_t slots_per_hyperframe)
{
  if (slots_per_hyperframe == 0) {
    return -1;
  }
  const uint64_t step = (slot + slots_per_hyperframe - (prev_slot % slots_per_hyperframe)) % slots_per_hyperframe;
  return prev_distance + static_cast<int64_t>(step);
}

/// The distance to \p slot, taken as the SHORTER of the two ways round the ring and added to the accumulator
/// when \p advance.
///
/// The shorter-way rule matters because two callers ask: the receive path, which walks forward one slot at a
/// time and is the one that ADVANCES the accumulator, and the lane, which may ask about a slot the receive path
/// has already passed (a hop that arrives late). Taking "forward" unconditionally would read a slot behind the
/// accumulator as almost a whole hyperframe AHEAD - a target 10.24 s in the future, which the clamp would wait
/// for. The lane therefore reads without advancing, so its question cannot move the stream's bookkeeping.
int64_t lane_grid_distance_of(uint64_t slot, bool advance)
{
  lane_grid_state&            st = lane_grid();
  std::lock_guard<std::mutex> lock(st.distance_mutex);
  const uint64_t forward  = (slot + kSlotsPerHyperframe - (st.last_slot % kSlotsPerHyperframe)) % kSlotsPerHyperframe;
  const uint64_t backward = (st.last_slot + kSlotsPerHyperframe - (slot % kSlotsPerHyperframe)) % kSlotsPerHyperframe;
  const int64_t  step     = (forward <= backward) ? static_cast<int64_t>(forward)
                                                  : -static_cast<int64_t>(backward);
  if (advance) {
    st.last_distance += step;
    st.last_slot      = slot;
    return st.last_distance;
  }
  return st.last_distance + step;
}

lane_grid_update lane_grid_update_ns(int64_t anchor_host_ns,
                                     int64_t anchor_slot,
                                     int64_t distance_slots,
                                     int64_t host_ns,
                                     int64_t slot_duration_ns,
                                     int64_t gain_shift,
                                     uint64_t slots_per_hyperframe)
{
  lane_grid_update out;
  (void)gain_shift;         // kept in the signature for the report's parameter list; the rule no longer walks
  (void)slots_per_hyperframe; // the distance arrives UNWRAPPED (see lane_grid_unwrap_distance)
  if (anchor_slot < 0) {
    // ARM once, on the first frontier: the grid is then the radio's cadence as the HOST saw it at that instant,
    // and it is extrapolated from there. Nothing about later arrivals moves its phase.
    out.anchor_host_ns = host_ns;
    out.anchor_slot    = static_cast<int64_t>(distance_slots);
    return out;
  }
  out.anchor_host_ns = anchor_host_ns;
  out.anchor_slot    = anchor_slot;
  const int64_t predicted = lane_grid_target_ns(anchor_host_ns, distance_slots - anchor_slot, slot_duration_ns, 0);
  if (predicted < 0) {
    return out; // an observation older than the anchor: nothing to correct and nothing to re-arm
  }
  if (predicted < 0) {
    return out;
  }
  // ---- THE OFFSET IS NOT WALKED, AND THAT IS THE DESIGN (2026-10-03, plan doc §11.20) -------------------
  //
  // The previous two versions walked the anchor toward the arrivals (gain 1/64, then 1/1024). Both were wrong
  // for the same reason in different sizes: walking the offset toward the arrivals ABSORBS THE DELIVERY LAG,
  // and the delivery lag is exactly what (a) the report exists to measure and (b) the thread's `lead` has to
  // cover. A grid that has absorbed it says "the lead is fine" by construction, which is how p224/p225 read
  // 0.65% late on a leg whose commits were 2 ms behind. The offset is now FIXED at the anchor and only the
  // RATE is corrected (from the radio's absolute time, in lane_grid_note_slot) - offset and rate are different
  // physical quantities from different sources, and only the second one belongs to the grid.
  //
  // The band below is therefore not a lag band: it catches only a DISPLACEMENT of the time base (a stream
  // restart, an epoch change). It has to be far wider than any lag excursion or it would swallow one - the
  // 8-slot band of the previous version would have re-armed on a 4 ms lag, i.e. exactly the bug above.
  const int64_t rearm_band = 1000000000LL; // 1 s
  if ((host_ns - predicted > rearm_band) || (predicted - host_ns > rearm_band)) {
    out.anchor_host_ns = host_ns;
    out.anchor_slot    = distance_slots; // the anchor moves to THIS observation, so its distance is the new zero
    out.rearmed        = true;
  }
  return out;
}

void lane_grid_note_slot(uint64_t slot, int64_t host_ns, int64_t radio_abs_ns)
{
#if defined(__APPLE__)
  if (!lane_grid_enabled()) {
    return;
  }
  lane_grid_state& st = lane_grid();
  // One observation per SLOT, even though the receive policy hands us a block per symbol: the filter's gain is
  // per observation, and fourteen of them per slot would make it fourteen times faster than documented.
  static std::atomic<int64_t> last_slot{-1};
  int64_t                     prev = last_slot.load(std::memory_order_relaxed);
  if (static_cast<int64_t>(slot) == prev) {
    return;
  }
  last_slot.store(static_cast<int64_t>(slot), std::memory_order_relaxed);

  if (radio_abs_ns >= 0) {
    if (!st.absolute_seen.exchange(true, std::memory_order_relaxed)) {
      st.abs_first_ns.store(radio_abs_ns, std::memory_order_relaxed);
      st.host_first_ns.store(host_ns, std::memory_order_relaxed);
    }
    st.abs_last_ns.store(radio_abs_ns, std::memory_order_relaxed);
    st.host_last_ns.store(host_ns, std::memory_order_relaxed);
    // The RATE estimate, and the guards are the interesting part. It needs a LONG baseline because each end
    // carries the transport lag of its own instant: 100 us of lag difference over 60 s of baseline is 1.7 ppm,
    // so anything shorter would fit the transport, not the clocks. It is clamped to +-200 ppm because a value
    // outside that is not a crystal pair, it is a broken time base - and applying it would walk the grid away
    // from the radio for the rest of the leg.
    const int64_t abs_span  = st.abs_last_ns.load(std::memory_order_relaxed) - st.abs_first_ns.load(std::memory_order_relaxed);
    const int64_t host_span = st.host_last_ns.load(std::memory_order_relaxed) - st.host_first_ns.load(std::memory_order_relaxed);
    if ((abs_span > 60000000000LL) && (host_span > 0)) {
      const double ppb = (static_cast<double>(host_span) / static_cast<double>(abs_span) - 1.0) * 1e9;
      if ((ppb > -200000.0) && (ppb < 200000.0)) {
        g_lane_grid_rate_ppb.store(static_cast<int64_t>(ppb), std::memory_order_relaxed);
      }
    }
  }
  const int64_t          anchor   = st.anchor_slot.load(std::memory_order_relaxed);
  const int64_t          anchor_h = st.anchor_host_ns.load(std::memory_order_relaxed);
  // THE LAG, before the filter moves the anchor: the residual against the grid the anchor already defines IS
  // the delivery lag (the grid instant is the radio's own cadence; the observation is when the host got it).
  const int64_t distance = lane_grid_distance_of(slot, /*advance=*/true);
  if (anchor >= 0) {
    const int64_t predicted = lane_grid_target_ns(anchor_h,
                                                  distance - anchor,
                                                  st.slot_duration_ns.load(std::memory_order_relaxed),
                                                  0);
    if (predicted >= 0) {
      const int64_t lag_us = (host_ns - predicted) / 1000;
      ++st.lag_n;
      if (st.lag_n == 1 || lag_us < st.lag_min_us.load(std::memory_order_relaxed)) {
        st.lag_min_us.store(lag_us, std::memory_order_relaxed);
      }
      if (lag_us > st.lag_max_us.load(std::memory_order_relaxed)) {
        st.lag_max_us.store(lag_us, std::memory_order_relaxed);
      }
      int64_t bucket = (lag_us - LAG_LO_US) / LAG_RES_US;
      if (bucket < 0) {
        bucket = 0;
      }
      else if (bucket >= static_cast<int64_t>(LAG_BUCKETS)) {
        bucket = LAG_BUCKETS - 1;
      }
      std::lock_guard<std::mutex> lock(st.lag_mutex);
      ++st.lag_buckets[bucket];
    }
  }
  const lane_grid_update up       = lane_grid_update_ns(anchor_h,
                                                        anchor,
                                                        distance,
                                                        host_ns,
                                                        st.slot_duration_ns.load(std::memory_order_relaxed),
                                                        st_gain_shift(),
                                                        kSlotsPerHyperframe);
  if ((up.anchor_host_ns != anchor_h) || (up.anchor_slot != anchor)) {
    st.anchor_host_ns.store(up.anchor_host_ns, std::memory_order_relaxed);
    st.anchor_slot.store(up.anchor_slot, std::memory_order_relaxed);
  }
  if (up.rearmed) {
    st.rearmed.fetch_add(1, std::memory_order_relaxed);
  }
  st.noted.fetch_add(1, std::memory_order_relaxed);
#else
  (void)slot;
  (void)host_ns;
#endif
}

int64_t lane_grid_next_tick_ns(int64_t prev_ns, int64_t slot_duration_ns)
{
#if defined(__APPLE__)
  if (slot_duration_ns <= 0) {
    return -1;
  }
  const lane_grid_state& st = lane_grid();
  if (st.anchor_slot.load(std::memory_order_relaxed) < 0) {
    return -1; // unarmed: the caller keeps its own cadence (see the executor's scope note)
  }
  const int64_t anchor = st.anchor_host_ns.load(std::memory_order_relaxed);
  // The first tick strictly after `prev_ns`, on the anchor's phase, with the same rate correction the lane's
  // clamp applies - so the thread's ticks and the clamp's instants are the same clock by construction.
  const int64_t elapsed = prev_ns - anchor;
  const int64_t steps   = (elapsed / slot_duration_ns) + 1;
  return anchor + steps * slot_duration_ns +
         static_cast<int64_t>((static_cast<double>(steps * slot_duration_ns) *
                               static_cast<double>(lane_grid_rate_ppb())) /
                              1e9);
#else
  // No grid on this platform, and nothing to ask: the paced executor's pacing switch is off there, so this is
  // unreachable - and returning -1 makes "unarmed" and "not applicable" the same answer, which is what the
  // caller's fallback already handles.
  (void)prev_ns;
  (void)slot_duration_ns;
  return -1;
#endif
}

void lane_grid_set_slot_duration_ns(int64_t ns)
{
#if defined(__APPLE__)
  if (ns > 0) {
    lane_grid().slot_duration_ns.store(ns, std::memory_order_relaxed);
  }
#else
  (void)ns; // no grid on this platform (see lane_grid_enabled): nothing to tell
#endif
}

void lane_grid_set_rate_ppb_for_test(int64_t ppb)
{
  g_lane_grid_rate_ppb.store(ppb, std::memory_order_relaxed);
}

void lane_grid_wait(uint64_t slot)
{
#if defined(__APPLE__)
  if (!lane_grid_enabled()) {
    return;
  }
  lane_grid_state& st       = lane_grid();
  const int64_t    anchor   = st.anchor_slot.load(std::memory_order_relaxed);
  const int64_t    anchor_h = st.anchor_host_ns.load(std::memory_order_relaxed);
  const int64_t    target   = lane_grid_target_ns(anchor_h,
                                                  lane_grid_distance_of(slot, /*advance=*/false) - anchor,
                                                  st.slot_duration_ns.load(std::memory_order_relaxed),
                                                  st.lead_ns.load(std::memory_order_relaxed));
  if (target < 0) {
    st.unarmed.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const int64_t now = steady_now_ns();
  if (now >= target) {
    // The grid instant has passed: the work did NOT get here in time, and no wait can fix that. Counted, not
    // hidden - this is the number that says whether the lead (and, at the next step, the reservation) is
    // enough. The commit is still made; skipping it would lose the slot's data for a scheduling reason.
    st.late.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const int64_t wait_ns = target - now;
  st.clamped.fetch_add(1, std::memory_order_relaxed);
  st.wait_sum_us.fetch_add(wait_ns / 1000, std::memory_order_relaxed);
  int64_t prev_max = st.wait_max_us.load(std::memory_order_relaxed);
  const int64_t wait_us = wait_ns / 1000;
  while ((wait_us > prev_max) &&
         !st.wait_max_us.compare_exchange_weak(prev_max, wait_us, std::memory_order_relaxed)) {
  }
  // Two phases, the shape the user's framework uses: SLEEP while there is time to spare (a sleep is free and
  // this thread may hold a pool thread), then SPIN the last stretch with the yield hint, because Darwin
  // coalesces short sleeps and a 50us quantum would leave tens of microseconds of jitter - exactly the
  // quantity this seam exists to remove.
  constexpr int64_t spin_tail_ns = 100000; // 100 us
  while (true) {
    const int64_t left = target - steady_now_ns();
    if (left <= 0) {
      break;
    }
    if (left > spin_tail_ns) {
      sprint_wait();
    }
    else {
      ::sched_yield();
    }
  }
  // ... and how far past the target we actually got there. A few microseconds means the clamp did its job; a
  // millisecond means the thread was descheduled inside its own clamp, and THAT is the quantity a reservation
  // can remove (this is the sub-millisecond scale the watchdog is blind to).
  const int64_t overshoot_us = (steady_now_ns() - target) / 1000;
  st.overshoot_sum_us.fetch_add(overshoot_us, std::memory_order_relaxed);
  int64_t prev_over = st.overshoot_max_us.load(std::memory_order_relaxed);
  while ((overshoot_us > prev_over) &&
         !st.overshoot_max_us.compare_exchange_weak(prev_over, overshoot_us, std::memory_order_relaxed)) {
  }
#else
  (void)slot;
#endif
}

void lane_grid_report()
{
#if defined(__APPLE__)
  lane_grid_report_impl();
#endif
}

void sprint_wait()
{
#if defined(__APPLE__)
  // 50 us: short enough that a buffer which arrives during the quantum is picked up within it, long enough that
  // an idle receive thread does not burn a core. The framework's idle lane uses the same idea.
  static const double ticks_per_us = [] {
    mach_timebase_info_data_t tb{};
    mach_timebase_info(&tb);
    return (tb.numer != 0) ? (static_cast<double>(tb.denom) / static_cast<double>(tb.numer)) : 24.0;
  }();
  const uint64_t now = mach_absolute_time();
  mach_wait_until(now + static_cast<uint64_t>(50.0 * ticks_per_us));
#else
  ::usleep(50);
#endif
}

uint16_t le16_to_host(uint16_t x)
{
#if defined(__APPLE__)
  return OSSwapLittleToHostInt16(x);
#else
  return x; // Linux targets are little-endian.
#endif
}

uint16_t host_to_le16(uint16_t x)
{
#if defined(__APPLE__)
  return OSSwapHostToLittleInt16(x);
#else
  return x;
#endif
}

uint32_t le32_to_host(uint32_t x)
{
#if defined(__APPLE__)
  return OSSwapLittleToHostInt32(x);
#else
  return x;
#endif
}

uint32_t host_to_le32(uint32_t x)
{
#if defined(__APPLE__)
  return OSSwapHostToLittleInt32(x);
#else
  return x;
#endif
}

size_t recommended_zmq_io_buf_bytes()
{
#if defined(__APPLE__)
  return 8u * 1024u * 1024u;
#else
  return 0;
#endif
}

void log_effective_decoder_backend(const std::string& decoder_type)
{
#if defined(__APPLE__)
  ocudulog::fetch_basic_logger("GNB").info("PUSCH LDPC decoder type: {}", decoder_type);
#else
  (void)decoder_type;
#endif
}

} // namespace compat

} // namespace ocudu
