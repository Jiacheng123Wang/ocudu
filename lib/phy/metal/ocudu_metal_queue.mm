// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu_metal_queue.h"

#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/support/macos_compat.h"

#include <atomic>
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

  /// Pending chain of one queue: the newest commit and how many are outstanding. Kept per queue
  /// because a wait on a command buffer of one queue cannot stand for the work of the other (see
  /// shared_queue::queue_kind).
  struct pending_chain {
    id<MTLCommandBuffer> last_committed = nil;
    uint64_t             pending        = 0;
  };
  static constexpr size_t nof_queue_kinds = 2;
  pending_chain           chains[nof_queue_kinds];

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
};

shared_queue_state& state();

#if defined(OCUDU_METAL_STATS)
void shared_queue_stats_report()
{
  shared_queue_state& s = state();
  ocudulog::fetch_basic_logger("PHY").debug("[metal_stats] wrap hits={} creates={} replaces={} failures={}",
               static_cast<unsigned long long>(s.wrap_hits),
               static_cast<unsigned long long>(s.wrap_creates),
               static_cast<unsigned long long>(s.wrap_replaces),
               static_cast<unsigned long long>(s.wrap_failures));
}

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

void shared_queue::notify_commit(id<MTLCommandBuffer> command_buffer, queue_kind kind)
{
  shared_queue_state& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  shared_queue_state::pending_chain& c = s.chain(kind);
  c.last_committed                     = command_buffer;
  ++c.pending;
  ++s.commits;
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
