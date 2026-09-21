// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu_metal_burst.h"
#include "ocudu_metal_lane_probe.h"
#include "ocudu_metal_queue.h"

#include "ocudu/ocudulog/ocudulog.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
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
  std::atomic<uint64_t> ce_dispatches{0};
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
               "(equalizer=%llu demapper=%llu channel_estimator=%llu)\n",
               static_cast<unsigned long long>(s.commits.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.waits.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.in_flight_max.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.dispatches.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.eq_dispatches.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.demap_dispatches.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.ce_dispatches.load(std::memory_order_relaxed)));
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
    // An ADOPTED command buffer (shared_burst::adopt()): it exists, its fences are already encoded, and
    // this is where its encoder opens - the point where encoder() also inserts the stage barrier.
    if (s.enc == nil) {
      s.enc = [s.cb computeCommandEncoder];
      if (s.enc == nil) {
        s.cb       = nil;
        s.pipeline = nil;
        return false;
      }
      s.pipeline = nil;
      s.n        = 0;
    }
    return true;
  }
  id<MTLCommandQueue> queue = shared_queue::backend_queue();
  if (queue == nil) {
    return false;
  }
  s.cb  = [queue commandBuffer];
  // Front-end fence (S-7g-17): the first stage that joins this burst may read the resource grid the
  // front-end DFTs produce (the equalizer does, and so does the estimator when it is fused into the
  // lane), and the two queues are independent. Encoded before the encoder opens, as the command-buffer
  // level API requires, and it covers every dispatch encoded into this burst afterwards.
  if (s.cb != nil) {
    shared_queue::front_end_wait(s.cb);
    // Back-end stage fence (S-7g-19, Step 1'): the lane burst reads what the ESTIMATOR wrote - the
    // weights, the per-symbol estimates and the noise variance - and the estimator wrote it into a
    // command buffer of its own, committed as soon as it was encoded so that its GPU work overlaps
    // the host encoding this burst. Two command buffers of one queue only have their STARTS ordered,
    // so without this wait the equalizer could read the estimator's memory before it is written. The
    // wait covers the whole burst, and it targets the estimator command buffer of THIS hop, which was
    // committed before this burst (the receiving chain estimates first, then demodulates).
    shared_queue::backend_stage_wait(s.cb);
  }
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

bool shared_burst::adopt(id<MTLCommandBuffer> cb)
{
  burst_state& s = state();
  if ((cb == nil) || (s.cb != nil)) {
    return false;
  }
  // The buffer only: the encoder opens on the first encoder() call, which is also where the stage
  // barrier lands (see burst_ensure_open()), and its command-buffer-level fences stay as the stages
  // inside it encoded them.
  s.cb       = cb;
  s.enc      = nil;
  s.pipeline = nil;
  s.n        = 0;
  return true;
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
  // The GPU-time probe must be armed before commit (Metal asserts otherwise).
  metal::shared_queue::arm_gpu_time(cb, metal::shared_queue::queue_kind::back_end);
  [cb commit];
  burst_stats_commit();
  // The burst is the command buffer whose completion produces the LLRs, i.e. the last one of the
  // lane: registering it here is what lets gpu_lane_probe attribute the residency to the stages that
  // were committed before it on this thread.
  gpu_lane_probe::register_commit(cb, gpu_lane_probe::stage::equalizer_demapper);
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
  // Everything the lane holds was committed before this burst on the same queue, so it completed
  // with it: the lane's metrics are final now.
  gpu_lane_probe::close_lane();
  return ok;
}

namespace {

/// A command buffer handed over by an earlier stage and not claimed yet (see shared_burst::deposit_released).
struct handed_entry {
  const void*          grid_base = nullptr;
  id<MTLCommandBuffer> cb        = nil;
};

/// Process-wide, because the two ends are two threads: the lower PHY (the radio thread) releases the block
/// its transforms went into, the upper PHY claims it when it starts the hop that reads that grid.
struct handed_state {
  std::mutex               mutex;
  std::deque<handed_entry> entries; // oldest first
  shared_burst::handed_counters counters;
};

handed_state& handed()
{
  // Never destroyed on purpose: the report runs from an atexit handler (see burst_stats_report), which runs
  // after the static destructors of this translation unit.
  static handed_state* s = new handed_state();
  return *s;
}

/// How many deposits are kept. The steady state is one per slot in flight, and a deposit is claimed by the
/// hop that reads its grid, so this is generous - it exists so that a hop that never runs (a slot with no
/// grant) cannot grow the list without bound.
constexpr size_t max_handed = 8;

} // namespace

void shared_burst::deposit_released(const void* grid_base, id<MTLCommandBuffer> cb)
{
  if ((grid_base == nullptr) || (cb == nil)) {
    return;
  }
  handed_state&               h = handed();
  std::lock_guard<std::mutex> lock(h.mutex);

  // One deposit per address: the same storage can come back through the grid pool for a LATER slot, and
  // that slot's own deposit is the one its consumer must take. The entry it replaces belongs to a slot
  // whose grid nobody read - the pool can only hand the address back once its holder let it go - which is
  // why this is counted apart from an eviction (see handed_counters).
  for (auto it = h.entries.begin(); it != h.entries.end(); ++it) {
    if (it->grid_base == grid_base) {
      it->cb = cb;
      ++h.counters.handed;
      ++h.counters.superseded;
      return;
    }
  }
  h.entries.push_back(handed_entry{grid_base, cb});
  ++h.counters.handed;
  while (h.entries.size() > max_handed) {
    // The oldest is the one whose consumer is least likely to still come. A backlog, not a recycle: the
    // [metal_stats] line reports it separately so that the expected case cannot hide a defect.
    h.entries.pop_front();
    ++h.counters.evicted;
  }
}

id<MTLCommandBuffer> shared_burst::take_released(const void* grid_base)
{
  if (grid_base == nullptr) {
    return nil;
  }
  handed_state&               h = handed();
  std::lock_guard<std::mutex> lock(h.mutex);
  for (auto it = h.entries.begin(); it != h.entries.end(); ++it) {
    if (it->grid_base == grid_base) {
      id<MTLCommandBuffer> cb = it->cb;
      h.entries.erase(it);
      ++h.counters.taken;
      return cb;
    }
  }
  return nil;
}

shared_burst::handed_counters shared_burst::handed_stats()
{
  handed_state&               h = handed();
  std::lock_guard<std::mutex> lock(h.mutex);
  handed_counters out   = h.counters;
  out.outstanding       = h.entries.size();
  return out;
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
  // The OPEN burst's own count: size() is what a stage - and the estimator's unit test - reads to check
  // that its dispatches really went into the shared command buffer instead of one of its own. Only the
  // open burst is counted: a dispatch of a stage that runs on its own command buffer is not part of any
  // lane, and burst_ensure_open() resets the count when the next burst opens anyway.
  burst_state& bs = state();
  if (bs.cb != nil) {
    ++bs.n;
  }
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
    case stage::channel_estimator:
      s.ce_dispatches.fetch_add(1, std::memory_order_relaxed);
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
