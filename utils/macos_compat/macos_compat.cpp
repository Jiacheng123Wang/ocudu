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
/// Default Mach real-time time constraint applied to real-time workers.
///
/// Period = computation = constraint = 1 ms matches the 5G subframe budget
/// (1 ms at 15 kHz SCS, 0.5 ms at 30 kHz): the worker declares a full-core
/// computational deadline every millisecond. The constraint is soft
/// (preemptible), so it never starves lower-priority work; the XNU scheduler
/// uses it to keep the thread on the performance cores and to protect it from
/// preemption by network I/O within its computation window.
constexpr darwin_thread_time_constraint default_rt_time_constraint{
    std::chrono::microseconds{1000},
    std::chrono::microseconds{1000},
    std::chrono::microseconds{1000},
    true};
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
  // Historical behaviour: pthread_setschedparam(SCHED_FIFO) is attempted on macOS as well (it succeeds without
  // privileges and is recorded by the kernel, even though the QoS class is the effective scheduling mechanism).
  return true;
#else
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
