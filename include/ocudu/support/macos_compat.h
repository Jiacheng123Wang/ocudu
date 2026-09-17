// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/support/executors/task_executor.h"
#include "ocudu/support/executors/unique_thread.h" // os_thread_realtime_priority, os_sched_affinity_bitmask
#include <cstddef>
#include <cstdint>
#include <future>
#include <pthread.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <vector>

namespace ocudu {
namespace compat {

/// \brief Cross-platform bottom layer that isolates OS and hardware differences.
///
/// The module is compiled on every platform (Linux and macOS) and maps the
/// platform-specific APIs (Mach vs POSIX) inside its own translation unit.
/// Business code only ever includes this header: the platform selection never
/// leaks into the callers, so the core libraries keep compiling as if they were
/// running on plain Linux.
///
/// Scope so far: memory management, high-resolution timing (Phase 1) and
/// thread/core scheduling (Phase 2). The scheduling functions aggregate the
/// Darwin implementation that lives in
/// \c ocudu/support/scheduling/darwin_thread_scheduling.{h,cpp} (kept in place);
/// on Linux they are no-ops because the POSIX priority (SCHED_FIFO) and CPU
/// pinning (pthread_setaffinity_np) applied by the thread wrapper are the
/// native mechanisms there.

// 1. Memory management.

/// \brief Allocates a block of \c size bytes with the given \c alignment.
///
/// Semantics:
/// - \c alignment is normalized to a power of two; any value below the
///   SIMD/cache-line minimum (64 bytes, covering AVX-512 and NEON loads) is
///   promoted to 64. A non-power-of-two alignment returns \c nullptr.
/// - \c size is rounded up to a multiple of the (normalized) alignment, so
///   arbitrary sizes are valid and the whole requested byte range is always
///   covered.
/// - The backing store is a plain heap allocation on both platforms today
///   (posix_memalign). Routing every hardware-accelerated buffer through this
///   API keeps the door open to swap the Apple implementation to a Metal UMA
///   shared-buffer / preallocated pool later without touching any caller.
///
/// \return Pointer to the aligned block, or \c nullptr on failure.
///         Release with aligned_free().
void* aligned_alloc(size_t alignment, size_t size);

/// \brief Releases memory allocated by aligned_alloc().
void aligned_free(void* ptr);

/// \brief Describes the aligned allocation a pointer belongs to.
///
/// Returns true and fills \p base / \p size when \p ptr lies inside a block handed out by
/// aligned_alloc(), and false when it does not (a static or non-aligned buffer, or a pointer that
/// was already released). Consumers that map host memory for a device - the Metal no-copy wrap
/// cache - need this to tell "a slice of the same allocation" from "a different allocation that
/// happens to share a page range": the two must never share one device resource.
///
/// \note The lookup is exact, including after a block has been released and its pages handed to a
/// new allocation: the registry entry is dropped by aligned_free().
bool describe_aligned_allocation(const void* ptr, void** base, size_t* size);

/// \brief Callback invoked by aligned_free() for the block it is about to release.
using aligned_free_observer = void (*)(void* base);

/// \brief Registers an observer of aligned_free().
///
/// For a consumer that keeps state keyed by the address of an allocation - the Metal no-copy wrap
/// cache, whose mappings are created per allocation - this is how that state is dropped when the
/// allocation dies. Without it the state outlives the memory: the pages handed to the next
/// allocation at the same address are then served by an object created for the previous one, which
/// is both a contract violation (a mapping must be created once per object) and a mapping whose
/// length describes a buffer that no longer exists.
///
/// Observers are process-wide and are called with \p base, the pointer aligned_alloc() returned,
/// before the block is released and after the registry entry is dropped. \p observer must not call
/// aligned_alloc(), aligned_free() or describe_aligned_allocation(): it runs between the two, on the
/// thread that is releasing the block. Register once, from a static initializer.
void register_aligned_free_observer(aligned_free_observer observer);

/// \brief Returns the operating system page size (4 KiB on x86 Linux,
///        16 KiB on Apple Silicon macOS).
size_t page_size();

// 2. High-resolution timing.

/// \brief Returns a monotonic wall-clock timestamp in microseconds.
///
/// The clock is unaffected by wall-clock adjustments (NTP/date changes):
/// - Linux: clock_gettime(CLOCK_MONOTONIC).
/// - macOS: mach_absolute_time() converted with the mach_timebase_info
///   conversion factor.
///
/// \return Monotonic time in microseconds, or 0 if the clock is unavailable.
uint64_t get_monotonic_time_us();

// 3. CPU locality.

/// \brief Returns the index of the CPU the calling thread is currently
///        running on.
///
/// - Linux: sched_getcpu() (0 when the syscall fails).
/// - macOS: no cheap per-thread CPU index is exposed to user space, so the
///   call returns 0 and the CPU-aware distribution in the memory pools
///   degenerates to a fixed offset (the behaviour previously inlined at the
///   call sites).
unsigned get_current_cpu();

/// \brief Returns the IDs of the CPUs this process is allowed to run on.
///
/// - Linux: the CPUs set in the process affinity mask (cpu_architecture_info).
/// - macOS: 0 .. hardware_concurrency()-1.
std::vector<size_t> get_available_cpu_ids();

// 4. Thread & core scheduling.
//
// Linux behaviour: all of the following are no-ops. The unique_thread wrapper
// applies the native POSIX mechanisms there (SCHED_FIFO via
// pthread_setschedparam, CPU pinning via pthread_setaffinity_np).
// macOS behaviour: the QoS class is the supported mechanism steering threads
// to the performance cores (P-cores); the Mach affinity tag co-locates threads
// that share a pipeline on the same L2 cluster; the Mach real-time time
// constraint (THREAD_TIME_CONSTRAINT_POLICY) declares a recurring CPU deadline
// to the XNU scheduler for the 1 ms / 0.5 ms subframe budget.

/// \brief Elevates the calling thread to the real-time (latency-critical)
///        scheduling class.
///
/// macOS: sets QOS_CLASS_USER_INTERACTIVE, which keeps the thread on the
///        performance cores. Linux: no-op.
void set_thread_realtime_priority();

/// \brief Requests that the calling thread stays on a performance core and
///        meets a real-time computational deadline.
///
/// macOS: QOS_CLASS_USER_INTERACTIVE plus a Mach real-time time constraint
///        (period = computation = constraint = 1 ms, preemptible). The
///        constraint is soft: it is attempted once per process and failures
///        are reported once, non-fatally (the QoS class remains in effect).
///        Linux: no-op.
void bind_thread_to_performance_core();

/// \brief Configures the pthread attributes before thread creation.
///
/// macOS: enlarges the default pthread stack (512 KiB) to 16 MiB so the gNB's
///        deep call chains (large TTI structures on the FAPI fastpath) do not
///        overflow it. Linux: no-op (the 8 MiB default is kept).
void configure_worker_thread_attributes(::pthread_attr_t& attr);

/// \brief Sets the name of the calling thread.
///
/// Hides the pthread_setname_np signature difference between the two
/// platforms (macOS takes only the name, Linux also takes the handle).
///
/// \return true on success.
bool set_thread_name(::pthread_t thread, const char* name);

/// \brief Applies the platform scheduling policy to a freshly created worker
///        thread. Must be called from within the target thread.
///
/// macOS: maps \c prio to a QoS class (USER_INTERACTIVE for the real-time
///        intent, USER_INITIATED otherwise) and applies the Mach affinity tag
///        derived from \c cpu_mask or \c thread_name (L2 cluster
///        co-location). This is the historical port behaviour; the Mach
///        time constraint is available as an explicit opt-in through
///        bind_thread_to_performance_core().
/// Linux: no-op.
void apply_worker_thread_scheduling(const os_thread_realtime_priority& prio,
                                    const os_sched_affinity_bitmask&  cpu_mask,
                                    std::string_view                  thread_name);

/// \brief Applies the CPU affinity mask to the given thread.
///
/// Linux: pthread_setaffinity_np; warns when the mask contains invalid CPU
///        IDs and returns false on failure. macOS: no-op, returns true (XNU
///        exposes no user-space pinning; the QoS class and affinity tag are
///        the scheduling mechanisms).
bool set_thread_affinity(::pthread_t                      thread,
                         const os_sched_affinity_bitmask& cpu_mask,
                         const std::string&               thread_name);

/// \brief Prints the current thread affinity to the console (debug aid).
///
/// Linux: prints the CPU set returned by pthread_getaffinity_np. macOS: prints
/// a notice that the query is not supported.
void print_thread_affinity_info(::pthread_t thread);

/// \brief Returns whether the POSIX real-time priority API (SCHED_FIFO via
///        pthread_setschedparam) is enforceable on this platform.
///
/// Always true: the historical port attempts pthread_setschedparam on macOS as
/// well (it succeeds without privileges; the QoS class remains the effective
/// scheduling mechanism there). The return value exists so the platform
/// decision stays inside the compat layer.
bool posix_realtime_priority_is_enforceable();

/// \brief Returns the real-time priority the radio (RU) worker should be
///        created with.
///
/// Linux: no_realtime() (upstream behaviour). macOS: max()-1, so the radio
/// channel loop (which moves the RF samples in/out of the baseband) is
/// elevated to the real-time QoS class.
os_thread_realtime_priority radio_worker_realtime_priority();

// 5. Networking (UDP) compat.

/// \brief GNU mmsghdr equivalent (macOS has no recvmmsg/sendmmsg and no mmsghdr type).
///
/// Layout-identical to the glibc definition; on Linux the recvmmsg/sendmmsg wrappers below pass it to the kernel
/// syscalls as ::mmsghdr.
struct mmsghdr {
  struct msghdr msg_hdr;
  unsigned int  msg_len;
};

/// MSG_WAITFORONE is a Linux-only recvmsg flag; define it (0) where it does not exist so the callers can use it
/// unconditionally.
#ifndef MSG_WAITFORONE
#define MSG_WAITFORONE 0
#endif

/// \brief sendmmsg: the kernel syscall on Linux; on macOS an emulation that sends every message with ::sendmsg.
/// \return Number of messages sent, or -1 on error.
int sendmmsg(int sockfd, mmsghdr* msgvec, unsigned vlen, int flags);

/// \brief recvmmsg: the kernel syscall on Linux; on macOS an emulation of the MSG_WAITFORONE semantics - block on
/// the first datagram, then drain with MSG_DONTWAIT until EAGAIN, so the receive callback never stalls one
/// inter-packet gap per datagram.
/// \return Number of datagrams received, or -1 on error.
int recvmmsg(int sockfd, mmsghdr* msgvec, unsigned vlen, int flags, void* timeout);

/// \brief msg_namelen to use in sendmsg().
///
/// Linux: sizeof(sockaddr_storage) (upstream behaviour). macOS: the exact family-specific length - macOS rejects
/// the full sockaddr_storage size for IPv6 destinations (EINVAL).
socklen_t sockaddr_length_for_send(const sockaddr_storage& addr);

// 6. Lower PHY / data-plane helpers.

/// \brief Completes the lower-PHY stop promise when the processing chain end is detected.
///
/// Linux (upstream behaviour): completes \c stop_control immediately - the stop finishes as soon as
/// on_process() detects the stop threshold. macOS: defers the completion to
/// lower_phy_stop_task_end(), so the stop only completes once the last processing task has actually finished
/// (this fixes the teardown deadlock of the deferred self-scheduling processing chains).
void lower_phy_stop_chain_end(std::promise<void>& stop_control);

/// \brief Called when a lower-PHY processing task finishes.
///
/// Linux: no-op. macOS: completes \c stop_control when the stop was requested and the FSM state has reached the
/// stop threshold (only the task that ended the sequential processing chain observes this condition).
void lower_phy_stop_task_end(uint32_t state, bool wait_stop, uint32_t state_stopped, std::promise<void>& stop_control);

/// \brief Waits until every task previously enqueued on the executor has finished.
///
/// Linux: no-op (the upstream stop semantics complete at the chain end). macOS: defers a sentinel task and waits
/// for it, so the processing executors are drained before the processor is destroyed.
/// \return true when the executor accepted the sentinel task (and the drain completed on macOS).
bool drain_executor_on_stop(task_executor& executor);

/// \brief Waits out one iteration of the lower-PHY DL tx-pacing loop.
///
/// Linux (upstream behaviour): sleeps 10 microseconds. macOS: spins with the YIELD hint - short sleeps are
/// coalesced by the Darwin scheduler and overshoot the 2 ms wall-clock deadline.
void wait_for_tx_timestamp();

/// \brief Host <-> little-endian byte-order conversions used by the MAC PDU decoders.
///
/// Linux: endian.h. macOS: libkern/OSByteOrder (OSSwap*).
uint16_t le16_to_host(uint16_t x);
uint16_t host_to_le16(uint16_t x);
uint32_t le32_to_host(uint32_t x);
uint32_t host_to_le32(uint32_t x);

/// \brief Recommended ZMQ socket buffer size (bytes) for the radio sample channels.
///
/// 0 = keep the ZMQ default (Linux, upstream behaviour). 8 MiB on macOS: a full baseband block (up to ~385 KB)
/// fits the advertised TCP window, so the peer's send/receive does not stall the lockstep channel loop.
size_t recommended_zmq_io_buf_bytes();

/// \brief Logs the effective PUSCH LDPC decoder backend at startup.
///
/// macOS: logs through the GNB logger (the expert_phy knob A/B runs are then verifiable from the startup logs).
/// Linux: no-op (upstream silent path).
void log_effective_decoder_backend(const std::string& decoder_type);

} // namespace compat
} // namespace ocudu
