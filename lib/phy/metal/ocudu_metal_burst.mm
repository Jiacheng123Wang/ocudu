// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu_metal_burst.h"
#include "ocudu_metal_lane_probe.h"
#include "ocudu_metal_queue.h"

#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/phy/phy_pipeline_grid_ready.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

using namespace ocudu;

namespace ocudu {
namespace metal {

/// How many hand-over records are kept. A record now lives until its grid has been PRODUCED (so a late
/// reader can be told "already written" rather than "unknown"), which is why this is a few slots of history
/// rather than a handful of entries: at ~1000 slots/s, 256 covers a quarter of a second - far more than the
/// one slot a consumer can be late by - and the bound still stops a hop that never runs from growing it.
constexpr size_t max_handed = 256;

/// \brief The Metal end of the host-reader hook (include/ocudu/phy/phy_pipeline_grid_ready.h).
///
/// A HOST consumer of the resource grid - the PUCCH in all its formats, the SRS - asks here before it reads,
/// and the answer covers both shapes of a slot: the lane's commit when a hop claimed the block, and a commit
/// the consumer performs itself when nobody did (a PUCCH-only slot, where nothing else would ever write that
/// grid). Installed once, on first use.
bool grid_ready_wait_hook(const void* storage, uint64_t slot, uint32_t timeout_ms)
{
  (void)timeout_ms; // the bound lives in ensure_grid_produced(); the registry owns the production fence
  return shared_burst::ensure_grid_produced(storage, slot);
}

/// \brief The Metal end of the hand-over's counters (see grid_handover_counts).
///
/// The same numbers the exit-time `[metal_stats] dft handover` line prints, but readable WHILE the run is
/// still going - which is the only way a harness can refuse to believe its own comparison after the
/// mechanism it meant to exercise did nothing (5.9.19).
void grid_handover_counts_hook(grid_handover_counts& out)
{
  const metal::shared_burst::handed_counters hand = metal::shared_burst::handed_stats();
  out.installed        = true;
  out.handed           = hand.handed;
  out.taken            = hand.taken;
  out.superseded       = hand.superseded;
  out.evicted          = hand.evicted;
  out.fallback_commits = hand.fallback_commits;
  out.late_commits     = hand.late_commits;
  out.not_found        = hand.grid_not_found;
  out.unproduced       = hand.unproduced;
  out.ready_timeouts   = hand.ready_timeouts;
}

/// \brief The Metal end of the non-blocking claim (see grid_ready_hook::claim()).
///
/// It IS grid_production_generation(): the device consumer's own entry point - claim the block, commit it
/// when nobody else will, and hand back the generation to encode a wait on. The harness reaches it through
/// the hook so that a plain C++ tool can reach it at all.
uint64_t grid_ready_claim_hook(const void* storage, uint64_t slot)
{
  return metal::shared_burst::grid_production_generation(storage, slot);
}

/// \brief The Metal end of the slot's hop plan (see slot_hop_plan_hook).
///
/// THREAD-LOCAL, and that is the point: a slot's hops run in order on one lane thread
/// (max_pusch_and_srs_concurrency is 1), so the plan of the slot being processed is exactly the plan of the
/// thread processing it. Layer 1 only records it - nothing reads it yet (design document 5.9.43).
namespace {
struct slot_hop_plan_t {
  uint64_t slot      = 0;
  unsigned hop_count = 0;
  unsigned hop_index = 0;
};
slot_hop_plan_t& slot_hop_plan()
{
  static thread_local slot_hop_plan_t plan;
  return plan;
}
} // namespace

void slot_hop_plan_set_hook(uint64_t slot, unsigned hop_count, unsigned hop_index)
{
  slot_hop_plan().slot      = slot;
  slot_hop_plan().hop_count = hop_count;
  slot_hop_plan().hop_index = hop_index;
}

const bool grid_ready_hook_installed = []() {
  slot_hop_plan_hook::install(&slot_hop_plan_set_hook);
  grid_ready_hook::install(&grid_ready_wait_hook);
  grid_ready_hook::install_counts(&grid_handover_counts_hook);
  grid_ready_hook::install_claim(&grid_ready_claim_hook);
  return true;
}();

namespace {

/// Per-thread burst state (see the header for why it is thread local).
struct burst_state {
  id<MTLCommandBuffer>              cb       = nil;
  id<MTLComputeCommandEncoder>      enc      = nil;
  id<MTLComputePipelineState>       pipeline = nil;
  unsigned                          n        = 0;
  void*                             flush_ctx  = nullptr;
  shared_burst::flush_hook_t        flush_hook = nullptr;
  /// Grid-production fence this thread's NEXT command buffer must wait for, before any of its dispatches
  /// (see shared_burst::set_grid_wait()). Consumed when the buffer is created.
  uint64_t                          grid_wait  = 0;
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
      // A grid wait that is still pending here was set AFTER a buffer was already open, so it cannot be
      // encoded before this burst's dispatches - the one shape that would silently drop the ordering. It is
      // left pending for the next buffer and counted by the caller (see mmse_engine's grid_wait_unencoded).
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
    // Grid production (D1): this burst may belong to a hop that MISSED the hand-over, in which case the
    // resource grid it is about to read is produced by a block committed by SOMEONE ELSE (another
    // consumer's fallback, or the registry's sweep) and the two are only ordered if the commit happens
    // first (see shared_burst::grid_production_generation()). Encoded here, where the buffer is created and
    // before any dispatch, because a command-buffer-level wait cannot be expressed inside an encoder - and
    // because the CPU thread must NOT wait for it (that is the stall of design document 5.9.23).
    if (s.grid_wait != 0) {
      (void)shared_queue::grid_ready_encode_wait(s.cb, s.grid_wait);
      s.grid_wait = 0;
    }
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

/// Lifecycle trace of a handed-over block (D1 diagnostics, 5.9.11): which command buffer the lane took over,
/// and which one it committed. Together with the deposit/done lines of the DFT engine and the take line of
/// the estimator, a block whose tokens never come back is missing exactly one of them - which is the
/// difference between "the adopter never committed it" and "it was committed and still did not complete".
static void d1_trace(const char* what, id<MTLCommandBuffer> cb)
{
  static std::atomic<unsigned> logged{0};
  // Silent under the CONTROL arm only, and capped at 64 lines either way: the lane's burst runs in every
  // mode, and a control leg does not need one line per hop. Since the knob's default moved to ON (5.9.49)
  // this trace is on by default - hence the cap, which is what keeps it from drowning an ordinary leg.
  if (!grid_handover_armed() || (logged.fetch_add(1, std::memory_order_relaxed) >= 64)) {
    return;
  }
  {
    std::fprintf(stderr, "[d1_handover] burst %s cb=%p\n", what, (__bridge const void*)cb);
  }
}

bool shared_burst::adopt(id<MTLCommandBuffer> cb)
{
  burst_state& s = state();
  if ((cb == nil) || (s.cb != nil)) {
    return false;
  }
  d1_trace("adopt", cb);
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
  d1_trace("commit", cb);
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
  /// The RECEIVING SLOT the grid belongs to. Half of the key, and the half that makes it unambiguous: the
  /// grid pool hands a storage address back as soon as the upper PHY drops its reference - i.e. when the
  /// next slot's grid arrives - so a consumer asking one slot late would otherwise be served the NEXT
  /// slot's block, commit and wait for that one, and read a grid nobody ever wrote (measured: 72% of the
  /// PUCCH reports unusable, design document 5.9.15).
  uint64_t             slot      = 0;
  id<MTLCommandBuffer> cb        = nil;
  /// Runs if this entry is dropped instead of claimed (see deposit_released()). Empty when the depositor has
  /// nothing to let go.
  std::function<void()> on_drop;
  /// Grid-production fence generation (see shared_queue::grid_ready_signal), armed by the depositor on the
  /// buffer that will carry the grid: what a HOST reader waits for (ensure_grid_produced()).
  uint64_t generation = 0;
  /// Whether a hop has taken this deposit. An unclaimed one is committed by whoever needs the grid first.
  bool claimed = false;
  /// Set by the completion handler. The record outlives the production so that a LATE reader still finds it
  /// and is told "already produced" instead of "unknown" (see ensure_grid_produced()).
  bool produced = false;
};

/// Process-wide, because the two ends are two threads: the lower PHY (the radio thread) releases the block
/// its transforms went into, the upper PHY claims it when it starts the hop that reads that grid - or the
/// consumer that reads the grid on the host claims and commits it itself.
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

/// The record of \p grid_base's \p slot, or nullptr.
static handed_entry* find_handed(handed_state& h, const void* grid_base, uint64_t slot)
{
  for (handed_entry& entry : h.entries) {
    if ((entry.grid_base == grid_base) && (entry.slot == slot)) {
      return &entry;
    }
  }
  return nullptr;
}

/// Marks the record of \p cb produced (its grid has been written). Called from the completion handler.
///
/// The record is KEPT: a reader that asks after the production must be able to tell "already written" from
/// "never heard of it", and only the record can say so. The bound evicts old records eventually.
static void mark_handed_produced(id<MTLCommandBuffer> cb)
{
  handed_state&               h = handed();
  std::lock_guard<std::mutex> lock(h.mutex);
  for (handed_entry& entry : h.entries) {
    if (entry.cb == cb) {
      entry.produced = true;
      return;
    }
  }
}

/// \brief Commits a block the registry is about to drop, because nobody ever claimed it.
///
/// A handed-over block is only ever committed by whoever claims it - a hop, or the host reader that needs
/// its grid. When the registry drops one that was never claimed (the storage came back, or the bound was
/// reached), nothing would commit it at all: its dispatches would never run, so its grid would never be
/// written AND the input tokens it carries would never be released. Committing it here costs one late
/// command buffer and removes that whole class of silence.
///
/// The commit itself is the DEPOSITOR's business (the DFT engine knows what a front-end commit owes), so it
/// is installed once through set_drop_committer().
/// Handed to the commit path: the registry has no business knowing what a commit owes, so the caller that
/// deposited the block supplies it (see shared_burst::set_drop_committer()).
static std::atomic<shared_burst::drop_commit_fn>& drop_committer()
{
  static std::atomic<shared_burst::drop_commit_fn> fn{nullptr};
  return fn;
}

static void commit_dropped(id<MTLCommandBuffer> cb)
{
  if (cb == nil) {
    return;
  }
  shared_burst::drop_commit_fn commit = drop_committer().load(std::memory_order_acquire);
  if (commit == nullptr) {
    return;
  }
  commit((__bridge void*)cb);
}

} // namespace

void shared_burst::set_drop_committer(drop_commit_fn fn)
{
  drop_committer().store(fn, std::memory_order_release);
}

void shared_burst::deposit_released(const void*          grid_base,
                                    uint64_t             slot,
                                    id<MTLCommandBuffer> cb,
                                    uint64_t             generation,
                                    std::function<void()> on_drop)
{
  if ((grid_base == nullptr) || (cb == nil)) {
    return;
  }
  // Everything that can run a completion handler or drop a last reference happens OUTSIDE the lock (see the
  // two notes on that below): this function collects what to do and does it at the end.
  std::vector<std::function<void()>> dropped;
  std::vector<id<MTLCommandBuffer>>  commit_late;
  std::vector<id<MTLCommandBuffer>>  released_after_unlock;
  {
    handed_state&               h = handed();
    std::lock_guard<std::mutex> lock(h.mutex);

    handed_entry* entry = find_handed(h, grid_base, slot);
    if (entry != nullptr) {
      // The same (storage, slot) deposited twice: replace. Only the OLD buffer is affected - and if nobody
      // claimed it, it must still be committed (see commit_dropped()).
      if (entry->on_drop) {
        dropped.push_back(std::move(entry->on_drop));
      }
      if (!entry->claimed && !entry->produced) {
        commit_late.push_back(entry->cb);
      }
      released_after_unlock.push_back(entry->cb);
      entry->cb         = cb;
      entry->on_drop    = std::move(on_drop);
      entry->generation = generation;
      entry->claimed    = false;
      entry->produced   = false;
      ++h.counters.handed;
      ++h.counters.superseded;
    } else {
      h.entries.push_back(handed_entry{grid_base, slot, cb, std::move(on_drop), generation, false, false});
      ++h.counters.handed;
    }

    while (h.entries.size() > max_handed) {
      // Prefer an entry that has already been produced: it is only kept so a late reader can be told so.
      size_t victim = 0;
      for (size_t i = 0; i != h.entries.size(); ++i) {
        if (h.entries[i].produced) {
          victim = i;
          break;
        }
      }
      if (h.entries[victim].on_drop) {
        dropped.push_back(std::move(h.entries[victim].on_drop));
      }
      if (!h.entries[victim].claimed && !h.entries[victim].produced) {
        commit_late.push_back(h.entries[victim].cb);
      }
      released_after_unlock.push_back(h.entries[victim].cb);
      h.entries.erase(h.entries.begin() + static_cast<std::ptrdiff_t>(victim));
      ++h.counters.evicted;
    }

    // ★ NOBODY CLAIMS A SLOT FOREVER. A deposit that no hop and no host reader ever asked for has no
    // consumer coming - and with the (storage, slot) key it can never be superseded either, because that
    // key is never deposited twice. Left alone it sits here holding the input tokens of its transforms, so
    // the receive buffers it keeps alive never come back: measured as handed=64 taken=39 fallback=21 with
    // keepalives=840/896 (four blocks' worth) and the pool at zero, which stalls the radio (5.9.17).
    //
    // The deadline is the receiving chain's own progress: a block whose slot is this far behind the newest
    // deposit has had its turn - the hop and the host readers of that slot are dispatched within a slot of
    // the symbols being reported. K = 2 slots is that window with room to spare, and it is the difference
    // between a pool of eight surviving (2-3 held) and dying.
    constexpr uint64_t sweep_after_slots = 2;
    for (handed_entry& entry : h.entries) {
      if (!entry.claimed && !entry.produced && ((entry.slot + sweep_after_slots) < slot)) {
        // Claimed here so a late hop cannot adopt a buffer that is about to be committed (encoding into a
        // committed buffer is an error): it opens one of its own and reads the grid the sweep writes.
        entry.claimed = true;
        commit_late.push_back(entry.cb);
        ++h.counters.late_commits;
      }
    }
  }
  // The record lives until the buffer COMPLETES: `no record` has to mean `nothing to wait for`, which is
  // what a host reader relies on (ensure_grid_produced()).
  [cb addCompletedHandler:^(id<MTLCommandBuffer> completed) { mark_handed_produced(completed); }];

  for (id<MTLCommandBuffer> late : commit_late) {
    commit_dropped(late);
  }
  for (const std::function<void()>& hook : dropped) {
    hook();
  }
  released_after_unlock.clear();
}

id<MTLCommandBuffer> shared_burst::take_released(const void* grid_base, uint64_t slot)
{
  if (grid_base == nullptr) {
    return nil;
  }
  handed_state&               h = handed();
  std::lock_guard<std::mutex> lock(h.mutex);
  handed_entry*               entry = find_handed(h, grid_base, slot);
  if ((entry == nullptr) || entry->claimed) {
    return nil;
  }
  entry->claimed = true;
  ++h.counters.taken;
  return entry->cb;
}

/// \brief The production promise of (\p grid_base, \p slot): commits it when nobody claimed it, and returns
///        the generation that will mark its completion (0 when there is no record, or none was armed).
///
/// Shared by the two consumers that need it, which differ only in HOW they wait: a host reader blocks on
/// the event (ensure_grid_produced()), a device consumer encodes the wait into its own command buffer
/// (grid_production_generation()). The fallback - a consumer committing a block no hop claimed - is the
/// same debt in both cases, and so is the counter that records it.
///
/// \param[out] to_commit Set to the block this call claimed on the caller's behalf; the caller commits it
///             OUTSIDE the lock (a commit can run completion handlers, which take this same lock).
static uint64_t
claim_grid_production(handed_state& h, const void* grid_base, uint64_t slot, id<MTLCommandBuffer> __strong& to_commit)
{
  handed_entry* entry = find_handed(h, grid_base, slot);
  if (entry == nullptr) {
    // No record at all: either nothing was ever handed over for this (storage, slot) - no hand-over in this
    // build or run - or it was produced long enough ago to be evicted. Counted, because a LATE reader that
    // cannot wait is exactly the case the key was introduced for.
    ++h.counters.grid_not_found;
    return 0;
  }
  if (!entry->claimed && !entry->produced) {
    // Nobody will ever commit this one (a slot no hop ran for), and a grid nobody produces is a grid the
    // caller is about to read as garbage: the fallback a hand-over owes.
    entry->claimed = true;
    to_commit      = entry->cb;
    ++h.counters.fallback_commits;
  }
  return entry->generation;
}

bool shared_burst::ensure_grid_produced(const void* grid_base, uint64_t slot)
{
  if (grid_base == nullptr) {
    return true;
  }
  id<MTLCommandBuffer> to_commit  = nil;
  uint64_t             generation = 0;
  {
    handed_state&               h = handed();
    std::lock_guard<std::mutex> lock(h.mutex);
    generation = claim_grid_production(h, grid_base, slot, to_commit);
  }
  if (to_commit != nil) {
    // A consumer had to commit it (fallback), which is counted apart from the registry's own late commits:
    // the first says a host reader found the block nobody claimed, the second that nobody came at all.
    commit_dropped(to_commit);
  }
  if (generation == 0) {
    return true;
  }
  // Bounded on purpose: a generation nobody signals must not hang the caller for good - and a timeout is
  // counted, because the caller then knows the grid is NOT trustworthy.
  constexpr uint32_t ready_timeout_ms = 200;
  if (!shared_queue::grid_ready_wait(generation, ready_timeout_ms)) {
    handed_state&               h = handed();
    std::lock_guard<std::mutex> lock(h.mutex);
    ++h.counters.ready_timeouts;
    return false;
  }
  return true;
}

uint64_t shared_burst::grid_production_generation(const void* grid_base, uint64_t slot)
{
  if (grid_base == nullptr) {
    return 0;
  }
  id<MTLCommandBuffer> to_commit  = nil;
  uint64_t             generation = 0;
  {
    handed_state&               h = handed();
    std::lock_guard<std::mutex> lock(h.mutex);
    generation = claim_grid_production(h, grid_base, slot, to_commit);
  }
  if (to_commit != nil) {
    commit_dropped(to_commit);
  }
  // The caller may also learn here that the block was already produced: the generation it gets then names a
  // value the event has reached, and encoding the wait on it is a satisfied wait rather than a mistake.
  return generation;
}

void shared_burst::set_grid_wait(uint64_t generation)
{
  state().grid_wait = generation;
}

bool shared_burst::grid_wait_pending()
{
  return state().grid_wait != 0;
}

shared_burst::handed_counters shared_burst::handed_stats()
{
  handed_state&               h = handed();
  std::lock_guard<std::mutex> lock(h.mutex);
  handed_counters out   = h.counters;
  for (const handed_entry& entry : h.entries) {
    out.unproduced += entry.produced ? 0u : 1u;
  }
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
