// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu/phy/phy_pipeline_contract.h"
#include "ocudu_metal_queue.h"

#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/support/macos_compat.h"

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

using namespace ocudu;

namespace ocudu {
namespace metal {

namespace {

struct shared_queue_state {
  id<MTLDevice>       device        = nil;
  id<MTLCommandQueue> queue         = nil;
  id<MTLCommandQueue> backend_queue = nil;

  std::mutex mutex;

  /// Grid-production fence (see shared_queue::grid_ready_signal): its own event, because it is waited on by
  /// the HOST.
  id<MTLSharedEvent>       grid_event    = nil;
  std::atomic<uint64_t>    grid_generation{0};

  /// Back-end stage fence (see the header): the estimator's own command buffer against the lane burst,
  /// with a generation of its own.
  id<MTLSharedEvent>       stage_fence_event = nil;
  std::atomic<uint64_t>    stage_fence_generation{0};
  std::atomic<uint64_t>    stage_fence_signals{0};
  std::atomic<uint64_t>    stage_fence_waits{0};
  std::atomic<uint64_t>    stage_fence_skipped_waits{0};

  /// One no-copy wrap: the buffer, the host range it covers, and the allocation it was made for.
  ///
  /// Address containment alone is not sound. The allocator hands the pages of a released block to
  /// the next allocation, so two different buffers can share a page range over time; and the stages
  /// of the chain ask for different ranges of ONE allocation (a group submit wraps the whole group,
  /// a per-symbol stage wraps a slice of it), so those must share the object. The allocation
  /// describes which case applies: compat::describe_aligned_allocation() answers it exactly, and a
  /// mapping is only reused for a request of the same allocation.
  struct wrap_entry {
    id<MTLBuffer> buffer = nil;
    /// First byte of the mapped range.
    const char* base = nullptr;
    /// Bytes the mapping covers (page rounded).
    size_t len = 0;
    /// Allocation the mapping was created for (null when the address belongs to no known block).
    const void* alloc = nullptr;

    /// True when a request at \p p may be served by this mapping.
    bool serves(const char* p, size_t aligned) const
    {
      if ((base == nullptr) || (p < base)) {
        return false;
      }
      if ((static_cast<size_t>(p - base) + aligned) > len) {
        return false;
      }
      if (alloc == nullptr) {
        return true;
      }
      void*       req_alloc = nullptr;
      size_t      req_size  = 0;
      const bool  known     = compat::describe_aligned_allocation(p, &req_alloc, &req_size);
      // An unknown request (a slice of a mapping, a non-allocator address) is left to the geometry
      // test above; a known one must belong to the very allocation this mapping was made for.
      return !known || (req_alloc == alloc);
    }
  };

  /// No-copy wraps shared by every engine (see shared_queue::wrap_no_copy), keyed by the address
  /// the mapping was created for.
  std::unordered_map<const void*, wrap_entry> wrap_cache;

  /// GPU execution time of the command buffers of one queue (see the [metal_stats] gpu busy report).
  struct gpu_time_stats {
    std::atomic<uint64_t> commits{0};
    /// Sum of the buffers' execution windows, in nanoseconds of the GPU timeline.
    std::atomic<uint64_t> busy_ns{0};
    /// First GPU start and last GPU end seen on this queue: their difference is the queue's GPU window.
    std::atomic<uint64_t> first_start_ns{0};
    std::atomic<uint64_t> last_end_ns{0};
  };
  /// Pending chain of one queue: the newest commit and how many are outstanding. Kept per queue
  /// because a wait on a command buffer of one queue cannot stand for the work of the other (see
  /// shared_queue::queue_kind).
  struct pending_chain {
    id<MTLCommandBuffer> last_committed = nil;
    uint64_t             pending        = 0;
  };
  static constexpr size_t nof_queue_kinds = 2;
  pending_chain           chains[nof_queue_kinds];
  gpu_time_stats          gpu_time[nof_queue_kinds];

  pending_chain& chain(shared_queue::queue_kind kind)
  {
    return chains[static_cast<size_t>(kind)];
  }

  uint64_t commits = 0;
  /// Zero-copy wrap-cache accounting (see the [metal_stats] report below): a hit means two stages
  /// of the chain bind the SAME Metal buffer object for one address, which is what relates their
  /// accesses to it; a replace means a stage asked for more than the cached mapping and got a
  /// different object instead.
  uint64_t wrap_hits     = 0;
  uint64_t wrap_creates  = 0;
  uint64_t wrap_replaces = 0;
  /// Requests the platform refused to map (the pointer is not page-aligned, or the mapping failed):
  /// the caller staged the buffer through a copy instead, which is correct but is not zero-copy.
  uint64_t wrap_failures = 0;
  /// Mappings dropped because the allocation they were made for was released (see
  /// purge_wrap_cache). Not a contract violation: the mapping of the NEXT allocation that gets those
  /// pages is a create, whereas without the purge the stale mapping would count as a replaced one.
  uint64_t wrap_purges = 0;
  /// Wrap requests whose slice offset did not satisfy the alignment the binding needs (a `float2`
  /// argument wants 8 bytes, a `float` 4, a `char` 1). A non-zero count means the zero-copy path is
  /// only "usually" aligned: the engine then stages a copy instead of binding a misaligned slice.
  std::atomic<uint64_t> wrap_misaligned{0};
};

shared_queue_state& state();

#if defined(OCUDU_METAL_STATS)
void shared_queue_stats_report()
{
  shared_queue_state& s = state();
  std::fprintf(stderr,
               "[metal_stats] wrap hits=%llu creates=%llu replaces=%llu failures=%llu misaligned=%llu purges=%llu\n",
               static_cast<unsigned long long>(s.wrap_hits),
               static_cast<unsigned long long>(s.wrap_creates),
               static_cast<unsigned long long>(s.wrap_replaces),
               static_cast<unsigned long long>(s.wrap_failures),
               static_cast<unsigned long long>(s.wrap_misaligned.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.wrap_purges));
  // The fence between the stages of the receiving chain: the estimator's own command buffer against the
  // lane burst. Printed unconditionally (a zero line says "this run had no such producer", which is how a
  // leg tells a mechanism that is off from one that never fired). The FRONT-END fence that used to be
  // printed above it was retired in 5.9.65 - see the note in the header.
  std::fprintf(stderr,
               "[metal_stats] lane fence signals=%llu waits=%llu skipped=%llu generation=%llu\n",
               static_cast<unsigned long long>(s.stage_fence_signals.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.stage_fence_waits.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.stage_fence_skipped_waits.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.stage_fence_generation.load(std::memory_order_relaxed)));
  // GPU busy time, measured on the command buffers themselves (GPUStartTime/GPUEndTime in their
  // completion handlers): this is the one time measurement that keeps its meaning once the stages are
  // fused into a single command buffer, where the per-stage host timestamps say nothing any more.
  // `busy` is the sum of the command buffers' execution windows, `window` the span from the first
  // start to the last end of the queue (the union: with overlapping buffers it is smaller than busy).
  for (size_t kind = 0; kind != shared_queue_state::nof_queue_kinds; ++kind) {
    const shared_queue_state::gpu_time_stats& g = s.gpu_time[kind];
    const uint64_t n = g.commits.load(std::memory_order_relaxed);
    const uint64_t busy_ns = g.busy_ns.load(std::memory_order_relaxed);
    const uint64_t first = g.first_start_ns.load(std::memory_order_relaxed);
    const uint64_t last  = g.last_end_ns.load(std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[metal_stats] gpu busy (%s): commits=%llu busy=%.1fus mean=%.2fus window=%.1fus\n",
                 (kind == 0) ? "front_end" : "back_end",
                 static_cast<unsigned long long>(n),
                 static_cast<double>(busy_ns) / 1e3,
                 (n != 0) ? (static_cast<double>(busy_ns) / 1e3 / static_cast<double>(n)) : 0.0,
                 (last > first) ? (static_cast<double>(last - first) / 1e3) : 0.0);
  }
}

/// \brief Registers the zero-copy requirement: a mapping is created once per object and never
/// replaced (a "replaced" wrap is the re-map the demapper's G5 leg removed).
const bool shared_queue_contract_registered = []() {
  register_phy_pipeline_check(
      {"zero-copy wraps", []() -> std::optional<bool> {
         shared_queue_state& s = state();
         std::fprintf(stderr,
                      "%llu hits, %llu creates, %llu replaces, %llu failures, %llu misaligned",
                      static_cast<unsigned long long>(s.wrap_hits),
                      static_cast<unsigned long long>(s.wrap_creates),
                      static_cast<unsigned long long>(s.wrap_replaces),
                      static_cast<unsigned long long>(s.wrap_failures),
                      static_cast<unsigned long long>(s.wrap_misaligned.load(std::memory_order_relaxed)));
         if (s.wrap_creates == 0 && s.wrap_hits == 0) {
           return std::nullopt; // nothing was wrapped in this run
         }
         return (s.wrap_replaces == 0) && (s.wrap_failures == 0) &&
                (s.wrap_misaligned.load(std::memory_order_relaxed) == 0);
       }});
  return true;
}();

const bool shared_queue_stats_registered = []() {
  std::atexit(shared_queue_stats_report);
  return true;
}();
#endif

shared_queue_state& state()
{
  static shared_queue_state s;
  return s;
}

/// \brief Drops the mappings created for an allocation that is about to be released.
///
/// A no-copy mapping is created for ONE allocation and its length describes that allocation (see
/// wrap_no_copy), so it must not outlive it: the allocator hands the pages of a released block to
/// the next allocation, and a mapping kept across that point serves the new buffer with an object
/// created for the old one - a mapping that describes memory which no longer exists. It is also what
/// the "zero-copy wraps" contract check sees as a REPLACED mapping, which is how the offline
/// deferred-chain test reported it: its test cases allocate and release grids of different sizes,
/// the allocator reuses one address, and the wrap of the new grid found the old grid's mapping.
void purge_wrap_cache(void* base)
{
  shared_queue_state& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  for (auto it = s.wrap_cache.begin(); it != s.wrap_cache.end();) {
    if (it->second.alloc == base) {
      it = s.wrap_cache.erase(it);
      ++s.wrap_purges;
    } else {
      ++it;
    }
  }
}

const bool shared_queue_free_observer_registered = []() {
  compat::register_aligned_free_observer(&purge_wrap_cache);
  return true;
}();

std::once_flag& init_flag()
{
  static std::once_flag f;
  return f;
}

} // namespace

id<MTLBuffer> shared_queue::wrap_no_copy(id<MTLDevice> device, const void* ptr, size_t length, size_t* offset)
{
  if ((device == nil) || (ptr == nullptr)) {
    return nil;
  }
  const size_t page    = compat::page_size();
  const size_t aligned = ((length + page - 1) / page) * page;

  const auto publish_offset = [offset](size_t value) {
    if (offset != nullptr) {
      *offset = value;
    }
  };

  shared_queue_state& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);

  // Offset mode (the caller asked for the offset): the LARGEST cached mapping that contains the
  // requested range wins, even when a smaller mapping exists for the very same address - a stale
  // small mapping from an earlier wrap must not shadow the group-wide one, or the stages would bind
  // different objects again.
  const char* const p = static_cast<const char*>(ptr);

  // The allocation the request starts in, when the allocator knows it (see wrap_entry::serves).
  void*  alloc_base = nullptr;
  size_t alloc_size = 0;
  const bool alloc_known = compat::describe_aligned_allocation(ptr, &alloc_base, &alloc_size);

  // The request must fit inside the allocation the pointer belongs to, and this is checked before
  // anything else touches the cache: a request that reaches past its allocation would otherwise
  // evict the mapping of an address that is perfectly valid, and then be handed a buffer SHORTER
  // than the range it asked for. The kernel bound to that buffer would index memory the object does
  // not back - silently, on the GPU, which is how a run that walked from one staging allocation
  // into the next produced wrong LLRs (see the run bound in demod_flush_hook). Refuse instead: the
  // caller stages a copy, and the failure is counted.
  if (alloc_known && (aligned > ((alloc_size + page - 1) / page) * page)) {
    ++s.wrap_failures;
    static std::atomic<bool> overshoot_logged{false};
    bool                     expected = false;
    if (overshoot_logged.compare_exchange_strong(expected, true)) {
      const size_t remaining = alloc_size - static_cast<size_t>(static_cast<const char*>(ptr) - static_cast<const char*>(alloc_base));
      ocudulog::fetch_basic_logger("PHY").error(
          "Metal: a no-copy wrap of {} bytes was requested at {}, but only {} bytes are left in its "
          "allocation; refusing the mapping instead of handing out a shorter buffer",
          aligned,
          ptr,
          remaining);
    }
    return nil;
  }

  // Containment lookup: the mapping created for the closest address at or below the request wins,
  // provided it covers the page-rounded request and belongs to the same allocation. The
  // largest-mapping rule is what the chained stages need; the allocation check is what keeps two
  // allocations that share a page range apart.
  const shared_queue_state::wrap_entry* best     = nullptr;
  size_t                                best_off = 0;
  for (const auto& entry : s.wrap_cache) {
    const shared_queue_state::wrap_entry& candidate = entry.second;
    if (!candidate.serves(p, aligned)) {
      continue;
    }
    if ((best == nullptr) || (candidate.base > best->base)) {
      best     = &candidate;
      best_off = static_cast<size_t>(p - candidate.base);
    }
  }
  if (best != nullptr) {
    ++s.wrap_hits;
    publish_offset(best_off);
    return best->buffer;
  }

  auto it = s.wrap_cache.find(ptr);
  if (it != s.wrap_cache.end()) {
    if (it->second.len >= aligned) {
      ++s.wrap_hits;
      publish_offset(0);
      return it->second.buffer;
    }
    // The cached mapping is smaller than what this call needs: replace it. The object handed out
    // so far stays alive (its owner and any command buffer referencing it retain it), so a stage
    // that wrapped the same address earlier keeps binding the older, smaller object - see the wrap
    // accounting in the [metal_stats] report.
    s.wrap_cache.erase(it);
    ++s.wrap_replaces;
  }
  // Map the whole allocation when the allocator knows it, so that a later request for a smaller
  // slice of the same allocation is served by this object instead of creating a second one.
  const size_t mapped_len = alloc_known ? ((alloc_size + page - 1) / page) * page : aligned;
  id<MTLBuffer> buf = [device newBufferWithBytesNoCopy:(void*)ptr
                                               length:mapped_len
                                              options:MTLResourceStorageModeShared
                                          deallocator:nil];
  if (buf == nil) {
    ++s.wrap_failures;
    return nil;
  }
  ++s.wrap_creates;
  publish_offset(0);
  s.wrap_cache[ptr] = shared_queue_state::wrap_entry{buf, p, mapped_len, alloc_known ? alloc_base : nullptr};
  return buf;
}

id<MTLDevice> shared_queue::device()
{
  std::call_once(init_flag(), []() {
    shared_queue_state& s = state();
    s.device              = MTLCreateSystemDefaultDevice();
    if (s.device == nil) {
      ocudulog::fetch_basic_logger("PHY").error("Metal: no Metal device available");
      return;
    }
    s.queue = [s.device newCommandQueue];
    if (s.queue == nil) {
      ocudulog::fetch_basic_logger("PHY").error("Metal: command queue creation failed");
    }
    s.backend_queue = [s.device newCommandQueue];
    if (s.backend_queue == nil) {
      ocudulog::fetch_basic_logger("PHY").error("Metal: back-end command queue creation failed");
    }
  });
  return state().device;
}

id<MTLCommandQueue> shared_queue::queue()
{
  (void)device(); // ensures the device and queue are initialized
  return state().queue;
}

id<MTLCommandQueue> shared_queue::backend_queue()
{
  (void)device(); // ensures the device and queues are initialized
  return state().backend_queue;
}

void shared_queue::notify_wrap_misaligned()
{
#if defined(OCUDU_METAL_STATS)
  state().wrap_misaligned.fetch_add(1, std::memory_order_relaxed);
#endif
}

void shared_queue::arm_gpu_time(id<MTLCommandBuffer> command_buffer, queue_kind kind)
{
#if defined(OCUDU_METAL_STATS)
  // Opt-in (OCUDU_METAL_GPU_TIME=1). A completion handler per command buffer is not free: the driver
  // dispatches a block for each of them (tens per slot), and that lands on the same submission path the
  // real-time uplink depends on. A measurement that perturbs what it measures is worse than no
  // measurement, so the probe stays off unless a run asks for it (the counters do not have this
  // problem: they are plain atomic increments on the existing path).
  if (std::getenv("OCUDU_METAL_GPU_TIME") == nullptr) {
    (void)command_buffer;
    (void)kind;
    return;
  }
  // The GPU's own view of the command buffer: GPUStartTime/GPUEndTime are only meaningful once it has
  // completed, so they are read in the completion handler. Metal REQUIRES the handler to be installed
  // BEFORE commit() ("Completed handler provided after commit call" is an assertion, not a warning),
  // which is why this is a separate call the engines make right before committing - a no-op in a build
  // without the probe, so the production submit path pays nothing.
  //
  // The handler runs on a Metal thread and must not take our lock: the fields are atomics, and the
  // min/max updates are CAS loops.
  shared_queue_state::gpu_time_stats* g = &state().gpu_time[static_cast<size_t>(kind)];
  [command_buffer addCompletedHandler:^(id<MTLCommandBuffer> cb) {
    const double start_s = cb.GPUStartTime;
    const double end_s   = cb.GPUEndTime;
    if (!(end_s > start_s)) {
      return;
    }
    const uint64_t start_ns = static_cast<uint64_t>(start_s * 1e9);
    const uint64_t end_ns   = static_cast<uint64_t>(end_s * 1e9);
    g->commits.fetch_add(1, std::memory_order_relaxed);
    g->busy_ns.fetch_add(end_ns - start_ns, std::memory_order_relaxed);
    uint64_t prev = g->first_start_ns.load(std::memory_order_relaxed);
    while ((prev == 0 || start_ns < prev) &&
           !g->first_start_ns.compare_exchange_weak(prev, start_ns, std::memory_order_relaxed)) {
    }
    prev = g->last_end_ns.load(std::memory_order_relaxed);
    while (end_ns > prev && !g->last_end_ns.compare_exchange_weak(prev, end_ns, std::memory_order_relaxed)) {
    }
  }];
#else
  (void)command_buffer;
  (void)kind;
#endif
}

void shared_queue::notify_commit(id<MTLCommandBuffer> command_buffer, queue_kind kind)
{
  shared_queue_state& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  shared_queue_state::pending_chain& c = s.chain(kind);
  c.last_committed                     = command_buffer;
  ++c.pending;
  ++s.commits;
}



uint64_t shared_queue::grid_ready_signal(id<MTLCommandBuffer> command_buffer)
{
  if (command_buffer == nil) {
    return 0;
  }
  shared_queue_state& s = state();
  if (s.grid_event == nil) {
    id<MTLDevice> device = shared_queue::device();
    if (device == nil) {
      return 0;
    }
    s.grid_event = [device newSharedEvent];
    if (s.grid_event == nil) {
      return 0;
    }
  }
  const uint64_t generation = s.grid_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
  [command_buffer encodeSignalEvent:s.grid_event value:generation];
  return generation;
}

bool shared_queue::grid_ready_wait(uint64_t generation, uint32_t timeout_ms)
{
  if (generation == 0) {
    return true;
  }
  shared_queue_state& s = state();
  if (s.grid_event == nil) {
    return true;
  }
  return [s.grid_event waitUntilSignaledValue:generation timeoutMS:timeout_ms];
}


bool shared_queue::grid_ready_encode_wait(id<MTLCommandBuffer> command_buffer, uint64_t generation)
{
  if ((command_buffer == nil) || (generation == 0)) {
    return false;
  }
  shared_queue_state& s = state();
  if (s.grid_event == nil) {
    // The event is created by the SIGNALLER (grid_ready_signal), so no grid production was ever armed in
    // this process: nothing will signal that value, and encoding the wait would hang the buffer.
    return false;
  }
  [command_buffer encodeWaitForEvent:s.grid_event value:generation];
  return true;
}






uint64_t shared_queue::backend_stage_signal(id<MTLCommandBuffer> command_buffer)
{
  if (command_buffer == nil) {
    return 0;
  }
  shared_queue_state& s = state();
  if (s.stage_fence_event == nil) {
    // Under the lock, unlike the front-end event: two threads that each created their own event would
    // signal one and wait on the other, and the wait would never fire. Costs one lock per estimator
    // hop, i.e. nothing next to the commit it belongs to. (device() does not take this mutex, so
    // calling it here cannot deadlock.)
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.stage_fence_event == nil) {
      id<MTLDevice> device = shared_queue::device();
      if (device == nil) {
        return 0;
      }
      s.stage_fence_event = [device newSharedEvent];
      if (s.stage_fence_event == nil) {
        return 0;
      }
    }
  }
  // Taken and encoded immediately before the commit of the command buffer that carries the work, for
  // the reason the front-end fence documents: a generation that has been handed out always has a
  // signaller on its way, so a wait for it can never hang.
  const uint64_t generation = s.stage_fence_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
  [command_buffer encodeSignalEvent:s.stage_fence_event value:generation];
  s.stage_fence_signals.fetch_add(1, std::memory_order_relaxed);
  return generation;
}

uint64_t shared_queue::backend_stage_generation()
{
  return state().stage_fence_generation.load(std::memory_order_acquire);
}

bool shared_queue::backend_stage_wait(id<MTLCommandBuffer> command_buffer)
{
  if (command_buffer == nil) {
    return false;
  }
  shared_queue_state& s          = state();
  const uint64_t      generation = s.stage_fence_generation.load(std::memory_order_acquire);
  if ((generation == 0) || (s.stage_fence_event == nil)) {
    // No estimator has committed in this process (the unit tests that drive the engine directly, the
    // replay tool, a configuration whose estimator ran synchronously): there is no signaller, and a
    // wait for a value nobody will signal would hang the command buffer.
    s.stage_fence_skipped_waits.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  [command_buffer encodeWaitForEvent:s.stage_fence_event value:generation];
  s.stage_fence_waits.fetch_add(1, std::memory_order_relaxed);
  return true;
}

uint64_t shared_queue::backend_stage_nof_signals()
{
  return state().stage_fence_signals.load(std::memory_order_relaxed);
}

uint64_t shared_queue::backend_stage_nof_waits()
{
  return state().stage_fence_waits.load(std::memory_order_relaxed);
}

bool shared_queue::backend_stage_wait_generation(id<MTLCommandBuffer> command_buffer, uint64_t generation)
{
  if ((command_buffer == nil) || (generation == 0)) {
    return false;
  }
  shared_queue_state& s = state();
  if (s.stage_fence_event == nil) {
    // Nothing was ever signalled in this process, so a wait for this value would never fire. Counted
    // the same way backend_stage_wait() counts its skips, and refused rather than encoded.
    s.stage_fence_skipped_waits.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  [command_buffer encodeWaitForEvent:s.stage_fence_event value:generation];
  s.stage_fence_waits.fetch_add(1, std::memory_order_relaxed);
  return true;
}

uint64_t shared_queue::backend_stage_nof_skipped_waits()
{
  return state().stage_fence_skipped_waits.load(std::memory_order_relaxed);
}

bool shared_queue::wait_all_committed(queue_kind kind)
{
  shared_queue_state& s = state();

  id<MTLCommandBuffer> cmd_buf = nil;
  uint64_t             pending = 0;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    shared_queue_state::pending_chain& c = s.chain(kind);
    cmd_buf                              = c.last_committed;
    pending                              = c.pending;
    c.pending                            = 0;
    c.last_committed                     = nil;
  }
  if ((pending == 0) || (cmd_buf == nil)) {
    return true;
  }

  [cmd_buf waitUntilCompleted];
  return cmd_buf.status == MTLCommandBufferStatusCompleted;
}

uint64_t shared_queue::nof_commits()
{
  shared_queue_state& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  return s.commits;
}

uint64_t shared_queue::nof_pending()
{
  shared_queue_state& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  uint64_t total = 0;
  for (const shared_queue_state::pending_chain& c : s.chains) {
    total += c.pending;
  }
  return total;
}

} // namespace metal
} // namespace ocudu
