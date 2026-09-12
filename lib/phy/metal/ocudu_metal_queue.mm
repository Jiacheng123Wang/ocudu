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
  id<MTLCommandBuffer>             last_committed = nil; // newest commit of the pending chain
  uint64_t                         commits        = 0;
  uint64_t                         pending        = 0;
};

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
      return it->second.first;
    }
    // The cached mapping is smaller than what this call needs: replace it.
    s.wrap_cache.erase(it);
  }
  id<MTLBuffer> buf = [device newBufferWithBytesNoCopy:(void*)ptr
                                               length:aligned
                                              options:MTLResourceStorageModeShared
                                          deallocator:nil];
  if (buf == nil) {
    return nil;
  }
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

void shared_queue::notify_commit(id<MTLCommandBuffer> command_buffer)
{
  shared_queue_state& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.last_committed = command_buffer;
  ++s.commits;
  ++s.pending;
}

bool shared_queue::wait_all_committed()
{
  shared_queue_state& s = state();

  id<MTLCommandBuffer> cmd_buf = nil;
  uint64_t             pending = 0;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    cmd_buf       = s.last_committed;
    pending       = s.pending;
    s.pending     = 0;
    s.last_committed = nil;
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
  return s.pending;
}

} // namespace metal
} // namespace ocudu
