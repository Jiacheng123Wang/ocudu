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
  void*                             flush_ctx  = nullptr;
  shared_burst::flush_hook_t        flush_hook = nullptr;
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
  std::atomic<uint64_t> eq_dispatches{0};
  std::atomic<uint64_t> demap_dispatches{0};
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
               "[metal_stats] burst commits=%llu waits=%llu max_in_flight=%llu dispatches=%llu "
               "(equalizer=%llu demapper=%llu)\n",
               static_cast<unsigned long long>(s.commits.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.waits.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.in_flight_max.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.dispatches.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.eq_dispatches.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.demap_dispatches.load(std::memory_order_relaxed)));
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

/// Creates the command buffer and its encoder when the burst is not open yet. Returns false when
/// the queue or the encoder could not be created; leaves the pipeline/number of dispatches alone.
static bool burst_ensure_open(burst_state& s)
{
  if (s.cb != nil) {
    return true;
  }
  id<MTLCommandQueue> queue = shared_queue::backend_queue();
  if (queue == nil) {
    return false;
  }
  s.cb  = [queue commandBuffer];
  s.enc = (s.cb != nil) ? [s.cb computeCommandEncoder] : nil;
  if (s.enc == nil) {
    s.cb       = nil;
    s.pipeline = nil;
    return false;
  }
  s.pipeline = nil;
  s.n        = 0;
  return true;
}

id<MTLComputeCommandEncoder> shared_burst::encoder(id<MTLComputePipelineState> pipeline)
{
  burst_state& s = state();
  if (!burst_ensure_open(s)) {
    return nil;
  }

  // A stage that accumulated dispatches (instead of encoding one per call) hands them over here,
  // before the pipeline comparison below decides whether a stage barrier is needed: the barrier
  // must land after those dispatches and before the next stage reads what they wrote.
  if (s.flush_hook != nullptr) {
    shared_burst::flush_hook_t  hook = s.flush_hook;
    void*                       ctx  = s.flush_ctx;
    s.flush_hook                     = nullptr;
    s.flush_ctx                      = nullptr;
    id<MTLComputePipelineState> flushed = hook(ctx, s.enc);
    if (flushed != nil) {
      s.pipeline = flushed;
    }
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
  burst_state& s = state();
  // A stage that accumulated dispatches (instead of encoding them) counts as an open burst even
  // before its first dispatch exists: the caller's wait() must still close and commit it.
  return (s.cb != nil) || (s.flush_hook != nullptr);
}

unsigned shared_burst::size()
{
  return state().n;
}

bool shared_burst::commit()
{
  burst_state& s = state();
  // Hand over the accumulated dispatches FIRST: a stage that encoded nothing per call still has to
  // hand its work over here (there is no later stage to trigger the pipeline change).
  if (s.flush_hook != nullptr) {
    if (!burst_ensure_open(s)) {
      return false;
    }
    shared_burst::flush_hook_t  hook = s.flush_hook;
    void*                       ctx  = s.flush_ctx;
    s.flush_hook                     = nullptr;
    s.flush_ctx                      = nullptr;
    id<MTLComputePipelineState> flushed = hook(ctx, s.enc);
    if (flushed != nil) {
      s.pipeline = flushed;
    }
  }
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

void shared_burst::set_flush_hook(void* context, flush_hook_t hook)
{
  burst_state& s = state();
  if ((s.flush_hook != nullptr) && ((s.flush_hook != hook) || (s.flush_ctx != context))) {
    // A different stage took over the registration: hand its pending work over before it can be
    // lost (the encoder may still be nil when nothing was encoded yet, which the hook tolerates).
    (void)shared_burst::flush_pending();
  }
  s.flush_ctx  = context;
  s.flush_hook = hook;
}

void* shared_burst::flush_hook_context()
{
  return state().flush_ctx;
}

id<MTLComputePipelineState> shared_burst::flush_pending()
{
  burst_state& s = state();
  if (s.flush_hook == nullptr) {
    return nil;
  }
  flush_hook_t                hook = s.flush_hook;
  void*                       ctx  = s.flush_ctx;
  s.flush_hook                     = nullptr;
  s.flush_ctx                      = nullptr;
  id<MTLComputePipelineState> flushed = hook(ctx, s.enc);
  if (flushed != nil) {
    // The dispatches stay in the burst and the stage tracking keeps their pipeline, so the barrier
    // of the next stage still lands after them.
    s.pipeline = flushed;
  }
  return flushed;
}

void shared_burst::count_dispatch(stage which)
{
#if defined(OCUDU_METAL_STATS)
  burst_stats_t& s = stats();
  s.dispatches.fetch_add(1, std::memory_order_relaxed);
  switch (which) {
    case stage::equalizer:
      s.eq_dispatches.fetch_add(1, std::memory_order_relaxed);
      break;
    case stage::demapper:
      s.demap_dispatches.fetch_add(1, std::memory_order_relaxed);
      break;
    case stage::other:
      break;
  }
#else
  (void)which;
#endif
}

} // namespace metal
} // namespace ocudu
