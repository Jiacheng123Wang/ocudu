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

  std::mutex                       mutex;
  /// No-copy wraps shared by every engine (see shared_queue::wrap_no_copy).
  std::unordered_map<const void*, std::pair<id<MTLBuffer>, size_t>> wrap_cache;

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
  std::fprintf(stderr, "[metal_stats] wrap hits=%llu creates=%llu replaces=%llu failures=%llu\n",
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

id<MTLBuffer> shared_queue::wrap_no_copy(id<MTLDevice> device, const void* ptr, size_t length)
{
  if ((device == nil) || (ptr == nullptr)) {
    return nil;
  }
  const size_t page    = compat::page_size();
  const size_t aligned = ((length + page - 1) / page) * page;

  shared_queue_state& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  auto                        it = s.wrap_cache.find(ptr);
  if (it != s.wrap_cache.end()) {
    if (it->second.second >= aligned) {
      ++s.wrap_hits;
      return it->second.first;
    }
    // The cached mapping is smaller than what this call needs: replace it. The object handed out
    // so far stays alive (its owner and any command buffer referencing it retain it), so a stage
    // that wrapped the same address earlier keeps binding the older, smaller object - see the wrap
    // accounting in the [metal_stats] report.
    s.wrap_cache.erase(it);
    ++s.wrap_replaces;
  }
  id<MTLBuffer> buf = [device newBufferWithBytesNoCopy:(void*)ptr
                                               length:aligned
                                              options:MTLResourceStorageModeShared
                                          deallocator:nil];
  if (buf == nil) {
    ++s.wrap_failures;
    return nil;
  }
  ++s.wrap_creates;
  s.wrap_cache[ptr] = std::make_pair(buf, aligned);
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
