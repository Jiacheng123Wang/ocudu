// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu_metal_burst.h"
#include "ocudu_metal_queue.h"

#include "ocudu/ocudulog/ocudulog.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace ocudu;

namespace ocudu {
namespace metal {

namespace {

/// Per-thread burst state (see the header for why it is thread local).
struct burst_state {
  id<MTLCommandBuffer>              cb       = nil;
  id<MTLComputeCommandEncoder>      enc      = nil;
  id<MTLComputePipelineState>       pipeline = nil;
  unsigned                          n        = 0;
  std::vector<id<MTLCommandBuffer>> outstanding; // committed through this thread, not waited yet

  ~burst_state()
  {
    // A thread that leaves with an open burst (an incomplete burst, or a stage that bailed out
    // before anything was committed) must still close its encoder: releasing it unfinished aborts
    // in the Metal validation layer.
    if (enc != nil) {
      [enc endEncoding];
      enc = nil;
    }
    cb = nil;
  }
};

burst_state& state()
{
  static thread_local burst_state s;
  return s;
}

/// Process-wide counters, printed once at exit when the probe is compiled in.
struct burst_stats_t {
  std::atomic<uint64_t> commits{0};
  std::atomic<uint64_t> waits{0};
  std::atomic<uint64_t> in_flight{0};
  std::atomic<uint64_t> in_flight_max{0};
  std::atomic<uint64_t> dispatches{0};
};

burst_stats_t& stats()
{
  static burst_stats_t s;
  return s;
}

#if defined(OCUDU_METAL_STATS)
void burst_stats_report()
{
  const burst_stats_t& s = stats();
  std::fprintf(stderr,
               "[metal_stats] burst commits=%llu waits=%llu max_in_flight=%llu dispatches=%llu\n",
               static_cast<unsigned long long>(s.commits.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.waits.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.in_flight_max.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.dispatches.load(std::memory_order_relaxed)));
}

void burst_stats_commit()
{
  burst_stats_t& s = stats();
  s.commits.fetch_add(1, std::memory_order_relaxed);
  const uint64_t nf = s.in_flight.fetch_add(1, std::memory_order_acq_rel) + 1;
  uint64_t       prev = s.in_flight_max.load(std::memory_order_relaxed);
  while (nf > prev && !s.in_flight_max.compare_exchange_weak(prev, nf, std::memory_order_relaxed)) {
  }
}

void burst_stats_wait()
{
  burst_stats_t& s = stats();
  s.waits.fetch_add(1, std::memory_order_relaxed);
  s.in_flight.fetch_sub(1, std::memory_order_acq_rel);
}

const bool burst_stats_registered = []() {
  std::atexit(burst_stats_report);
  return true;
}();
#else
void burst_stats_commit() {}
void burst_stats_wait() {}
#endif

} // namespace

id<MTLComputeCommandEncoder> shared_burst::encoder(id<MTLComputePipelineState> pipeline)
{
  burst_state& s = state();
  if (s.cb == nil) {
    id<MTLCommandQueue> queue = shared_queue::backend_queue();
    if (queue == nil) {
      return nil;
    }
    s.cb  = [queue commandBuffer];
    s.enc = (s.cb != nil) ? [s.cb computeCommandEncoder] : nil;
    if (s.enc == nil) {
      s.cb       = nil;
      s.pipeline = nil;
      return nil;
    }
    s.pipeline = nil;
    s.n        = 0;
  }

  if (s.pipeline != pipeline) {
    if (s.pipeline != nil) {
      // Stage boundary: the dispatches encoded so far (for example the equalization of a group)
      // wrote memory that the dispatches of the next stage (the demapping) read through a
      // different buffer object, so order them explicitly.
      [s.enc memoryBarrierWithScope:MTLBarrierScopeBuffers];
    }
    [s.enc setComputePipelineState:pipeline];
    s.pipeline = pipeline;
  }
  return s.enc;
}

bool shared_burst::open()
{
  return state().cb != nil;
}

unsigned shared_burst::size()
{
  return state().n;
}

bool shared_burst::commit()
{
  burst_state& s = state();
  if (s.cb == nil) {
    return false;
  }
  id<MTLCommandBuffer>         cb  = s.cb;
  id<MTLComputeCommandEncoder> enc = s.enc;
  s.cb                             = nil;
  s.enc                            = nil;
  s.pipeline                       = nil;
  s.n                              = 0;

  [enc endEncoding];
  [cb commit];
  burst_stats_commit();
  s.outstanding.push_back(cb);
  return true;
}

bool shared_burst::wait_committed()
{
  burst_state& s = state();
  if (s.outstanding.empty()) {
    return true;
  }
  std::vector<id<MTLCommandBuffer>> outstanding;
  outstanding.swap(s.outstanding);

  bool ok = true;
  for (id<MTLCommandBuffer> cb : outstanding) {
    burst_stats_wait();
    [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted) {
      ocudulog::fetch_basic_logger("PHY").error("Metal burst: command buffer failed with status {}",
                                                static_cast<unsigned long>(cb.status));
      ok = false;
    }
  }
  return ok;
}

void shared_burst::count_dispatch()
{
#if defined(OCUDU_METAL_STATS)
  stats().dispatches.fetch_add(1, std::memory_order_relaxed);
#endif
}

} // namespace metal
} // namespace ocudu
