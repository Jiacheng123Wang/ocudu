// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu_metal_burst.h"
#include "ocudu_metal_lane_clock.h"
#include "ocudu_metal_lane_probe.h"
#include "ocudu_metal_queue.h"
#include "ocudu/phy/phy_pipeline_report.h"

#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/phy/phy_pipeline_grid_ready.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
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
///
/// Read PER CALL rather than cached: OCUDU_D1_HANDED_BOUND is a diagnostic override and the arm that uses it
/// has to move the bound inside one process (see the header for why the bound cannot be reached otherwise).
size_t shared_burst::handed_capacity()
{
  const char* env = std::getenv("OCUDU_D1_HANDED_BOUND");
  if (env == nullptr) {
    return 256;
  }
  const unsigned long value = std::strtoul(env, nullptr, 10);
  return (value != 0) ? static_cast<size_t>(value) : 256;
}

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
  out.evicted_unproduced = hand.evicted_unproduced;
  out.over_bound         = hand.over_bound;
  out.fallback_commits = hand.fallback_commits;
  out.late_commits     = hand.late_commits;
  out.late_commits_time = hand.late_commits_time;
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

/// \brief The Metal end of the pool-dry reap hook (include/ocudu/phy/phy_pipeline_grid_ready.h, Q9-A).
///
/// The LOWER PHY calls this from the thread that is about to park on an empty receive pool. It is the one
/// caller the registry cannot reach on its own: a parked receive thread deposits nothing, so the "check at
/// every entry point" rule has no entry point to run at, and the blocks holding the pool stay unclaimed for
/// as long as the stall lasts (measured on `p08-conc2`: 5.945 s).
void handover_reap_hook_impl(handover_reap_hook::reap_reason why)
{
  shared_burst::reap_unclaimed_now(why);
}

const bool grid_ready_hook_installed = []() {
  slot_hop_plan_hook::install(&slot_hop_plan_set_hook);
  grid_ready_hook::install(&grid_ready_wait_hook);
  grid_ready_hook::install_counts(&grid_handover_counts_hook);
  grid_ready_hook::install_claim(&grid_ready_claim_hook);
  handover_reap_hook::install(&handover_reap_hook_impl);
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
  /// Q9-C: the stage-fence generation THIS thread's hop handed out (see set_stage_wait()). Consumed by the
  /// next burst_ensure_open(); 0 = nothing published, which keeps the old "newest generation" rule.
  uint64_t                          stage_wait = 0;
  std::vector<id<MTLCommandBuffer>> outstanding; // committed through this thread, not waited yet
  /// What the buffer this burst commits carries, for the lane probe's busy split (see set_commit_label()).
  ocudu::metal::gpu_lane_probe::stage commit_label = ocudu::metal::gpu_lane_probe::stage::equalizer_demapper;

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
  register_p0_report(burst_stats_report);
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
  if (s.cb != nil) {
    // Back-end stage fence (S-7g-19, Step 1'): the lane burst reads what the ESTIMATOR wrote - the
    // weights, the per-symbol estimates and the noise variance - and the estimator wrote it into a
    // command buffer of its own, committed as soon as it was encoded so that its GPU work overlaps
    // the host encoding this burst. Two command buffers of one queue only have their STARTS ordered,
    // so without this wait the equalizer could read the estimator's memory before it is written. The
    // wait covers the whole burst, and it targets the estimator command buffer of THIS hop, which was
    // committed before this burst (the receiving chain estimates first, then demodulates).
    // Q9-C: wait for THIS hop's own estimator generation when the estimator published one (it always does on
    // the routes that commit early - see mmse_engine: the signal and set_stage_wait() are issued together,
    // immediately before the commit that puts it ahead of this burst on the same queue). The global newest is
    // only the fallback for a route whose estimator ran synchronously on another thread or not at all.
    {
      const uint64_t own_generation = s.stage_wait;
      s.stage_wait                  = 0;
      if (own_generation != 0) {
        const bool crossed = shared_queue::backend_stage_generation() != own_generation;
        (void)shared_queue::backend_stage_wait_generation(s.cb, own_generation);
        shared_queue::note_stage_fence_wait(/*own_generation=*/true, crossed);
      } else {
        (void)shared_queue::backend_stage_wait(s.cb);
        shared_queue::note_stage_fence_wait(/*own_generation=*/false, /*crossed=*/false);
      }
    }
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
  // The PHY logger at DEBUG level, like the rest of this family (5.9.69, 5.9.72): these were the last raw
  // stderr lines on the console. The cap below still counts CALLS rather than prints, so a leg that never
  // raises the level spends 64 relaxed increments and nothing else.
  //
  // The trade-off, stated where it bites: a leg whose chain breaks has no exit report, and these lines are how
  // the handshake's two ends were compared (5.9.11). That is what `--log.phy_level=debug` is for - the leg
  // being DIAGNOSED runs with it, rather than every leg printing 256 lines to keep the option open.
  auto& logger = ocudulog::fetch_basic_logger("PHY");
  if (logger.debug.enabled()) {
    logger.debug("[d1_handover] burst {} cb={}", what, fmt::ptr((__bridge const void*)cb));
  }
}

/// P0-1 (doc_chinese/phy_latency/gpu_phy_latency_optimization_design_and_implementation.md 6.2): the DIAGNOSTIC SPLIT.
///
/// On the production route the whole hop - the front end's transforms, the estimator, the equalization
/// and the demapping - is ONE command buffer, and Metal hands out GPU timestamps only per command
/// buffer, so `merged_hop` (~1030us, 97% of the lane's busy time) cannot be separated into its stages.
/// Per-dispatch counters are not available on this device (5.8.15 (1): only AtStageBoundary is
/// supported). This knob is the remaining way to read them: it stops adopting the front end's buffer,
/// COMMITS it as its own submission - so its GPU span is readable as the probe's `dft` stage, a stage
/// that has existed in gpu_lane_probe but was never registered - and continues the hop in a fresh
/// buffer ordered after it by a shared event.
///
/// OFF by default, and when off nothing about the submission changes. When ON the arm is a DIAGNOSTIC,
/// not a delivery: a hop then costs two command buffers per lane instead of one (so its `cbs/lane`
/// reads ~2 and V4 does not apply to it), and the front end's input buffers are released earlier
/// because its command buffer completes earlier - which is P2-E's subject, so an arm with this knob on
/// must not be compared against a merged arm for anything except the GPU-time split it exists to read.
static bool diag_split_enabled()
{
  static const bool on = []() {
    const char* v = std::getenv("OCUDU_LANE_DIAG_SPLIT");
    return (v != nullptr) && (std::atoi(v) != 0);
  }();
  return on;
}

/// The event the diagnostic split orders its two buffers with: the front end's buffer signals it, the
/// estimator's buffer waits for it. Process-wide and generation-counted, like the stage fence, because
/// the split only ever orders a buffer against the one just committed on the SAME thread.
static id<MTLSharedEvent> diag_split_event()
{
  static id<MTLSharedEvent> ev = nil;
  static std::once_flag     once;
  std::call_once(once, []() {
    id<MTLCommandQueue> q = shared_queue::backend_queue();
    ev                    = (q != nil) ? [q.device newSharedEvent] : nil;
  });
  return ev;
}

bool shared_burst::adopt(id<MTLCommandBuffer> cb)
{
  burst_state& s = state();
  if ((cb == nil) || (s.cb != nil)) {
    return false;
  }
  if (diag_split_enabled()) {
    id<MTLSharedEvent>  ev = diag_split_event();
    id<MTLCommandQueue> q  = shared_queue::backend_queue();
    // The REPLACEMENT buffer is opened BEFORE the front end's is committed. Measured 2026-09-24 by
    // read-only audit: the first version committed first and created second, so a failure to create the
    // replacement (resource exhaustion) fell through to the normal path with s.cb holding an
    // ALREADY-COMMITTED buffer - the caller would encode into it and the lane would commit it a second
    // time, which is a hard Metal error. The call site's own contract is safe (mmse_engine commits st.cb
    // only when adopt() returns false), so the defect lived purely on the creation-failure path, and a
    // diagnostic arm must degrade to the production shape rather than break the hop.
    id<MTLCommandBuffer> nb = ((ev != nil) && (q != nil)) ? [q commandBuffer] : nil;
    if (nb != nil) {
      static std::atomic<uint64_t> gen{0};
      const uint64_t               value = gen.fetch_add(1, std::memory_order_relaxed) + 1;
      // Order the two buffers, then commit the front end's one: a command-buffer-level signal has to be
      // encoded while the buffer is still open, and the GPU-time probe must be armed before the commit.
      [cb encodeSignalEvent:ev value:value];
      shared_queue::arm_gpu_time(cb, shared_queue::queue_kind::back_end, "split_dft");
      gpu_lane_probe::register_commit(cb, gpu_lane_probe::stage::dft);
      shared_queue::note_commit_order(cb);
      [cb commit];
      shared_burst::note_block_commit_issued(cb);
      burst_stats_commit();
      s.outstanding.push_back(cb);
      [nb encodeWaitForEvent:ev value:value];
      d1_trace("adopt-split", nb);
      s.cb       = nb;
      s.enc      = nil;
      s.pipeline = nil;
      s.n        = 0;
      return true;
    }
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

void shared_burst::set_commit_label(gpu_lane_probe::stage which)
{
  state().commit_label = which;
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
  // The GPU-time probe must be armed before commit (Metal asserts otherwise), and the commit ticket (Q9-F) is
  // taken immediately before it: this buffer can carry the stage fence's wait and the grid-production wait, so
  // the order between its commit and its signaller's is the reading that says whether the wait can resolve.
  const char* burst_label = "lane_burst";
#if defined(OCUDU_METAL_STATS)
  if (s.commit_label == gpu_lane_probe::stage::merged_hop) {
    // The label names what the buffer CARRIES (Q9-F3's occupancy report reads it against the slots): on the
    // merged route the adopted block brings the front end's transforms with it, so "the lane's burst" is the
    // whole hop - the distinction the lane probe's busy split exists for.
    burst_label = "merged_hop";
  }
#endif
  metal::shared_queue::arm_gpu_time(cb, metal::shared_queue::queue_kind::back_end, burst_label);
  metal::shared_queue::note_commit_order(cb);
  // Dev doc 6.95, the tail mark: this is the hop's LAST host act - after it the CPU stands aside (G2). The
  // lane clock turns it into the span from the stage entry that opened this lane, i.e. the CPU's whole
  // participation in the hop (the head of it is handover_us, entry -> extraction commit). It must be taken
  // with the buffer still open and immediately before the commit, so the span covers the encoding too.
  metal::lane_clock.mark_lane_commit();
  [cb commit];
  // Dev doc 6.20: if this buffer is a handed-over block (the merged route adopts one), its commit is what a
  // consumer of that grid may order itself against - published here, AFTER the commit.
  shared_burst::note_block_commit_issued(cb);
  burst_stats_commit();
  // The burst is the command buffer whose completion produces the LLRs, i.e. the last one of the
  // lane: registering it here is what lets gpu_lane_probe attribute the residency to the stages that
  // were committed before it on this thread.
  gpu_lane_probe::register_commit(cb, s.commit_label);
  // Consumed: the next burst of this thread is the ordinary one unless it says otherwise again.
  s.commit_label = gpu_lane_probe::stage::equalizer_demapper;
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
  /// \brief Whether the command buffer that carries this block's grid-production signal has been COMMITTED.
  ///
  /// Set by the party that commits the block, right after `[cb commit]` (see
  /// shared_burst::note_block_commit_issued()). A device-side wait must not be encoded for a generation whose
  /// carrier is not in the queue yet: Metal releases an unsatisfied wait only after 5.00 s and holds the whole
  /// queue until then (dev doc 6.20, measured offline).
  bool commit_issued = false;
  /// Set by the completion handler. The record outlives the production so that a LATE reader still finds it
  /// and is told "already produced" instead of "unknown" (see ensure_grid_produced()).
  bool produced = false;
  /// \name P0-7: this block's own timeline, on the host's steady clock (see handed_counters).
  ///@{
  std::chrono::steady_clock::time_point deposited_at{};
  /// Deposit -> the moment SOMEONE claimed it (a hop, or the sweep), 0 while unclaimed.
  uint64_t claim_wait_us = 0;
  /// Whether the sweep (not a hop) was the one that claimed it.
  bool swept = false;
  /// Deposit -> completion, 0 while not produced.
  uint64_t produced_wait_us = 0;
  /// \brief The moment the REGISTRY decided to commit this block (Q9-B), or the zero time when it did not.
  ///
  /// The registry commits a block itself in exactly three cases (the sweep, a host reader's fallback, and
  /// the drop of a superseded entry) and hands the commit to the depositor's committer; a block a HOP claimed
  /// is committed by the lane and never passes through here, so this stays zero for it. That is what makes
  /// the pair below readable: a block with a commit time here whose completion was late was late *after* a
  /// commit the registry had already issued - while a block without one waited for its claimer, which is a
  /// different defect.
  std::chrono::steady_clock::time_point commit_issued_at{};
  /// Commit -> completion, in microseconds, 0 while the registry has not committed it or it is not produced.
  uint64_t commit_wait_us = 0;
  ///@}
};

/// Process-wide, because the two ends are two threads: the lower PHY (the radio thread) releases the block
/// its transforms went into, the upper PHY claims it when it starts the hop that reads that grid - or the
/// consumer that reads the grid on the host claims and commits it itself.
struct handed_state {
  std::mutex                   mutex;
  std::deque<handed_entry>     entries; // oldest first
  /// Slot of the newest deposit: the reference the sweep's SLOT window is measured against. Not the entry
  /// point's own slot - a host reader asks about the slot it READS, which can be several slots behind the
  /// receiving chain, and a take asks about the hop's slot, which is one behind by construction.
  uint64_t                     newest_slot = 0;
  shared_burst::handed_counters counters;
};

handed_state& handed()
{
  // Never destroyed on purpose: the report runs from an atexit handler (see burst_stats_report), which runs
  // after the static destructors of this translation unit.
  static handed_state* s = new handed_state();
  return *s;
}

/// \name Q9: the registry's deadline for a block NOBODY claims (dev doc 6.10).
///
/// WHY IT EXISTS AT ALL. A deposit no hop and no host reader ever asks for has no consumer coming, and with the
/// (storage, slot) key it can never be superseded either. It therefore sits in the registry holding the input
/// tokens of its transforms, so the receive buffers those keep alive never come back - which is why the sweep
/// below is what stands between one missed hand-over and a stalled radio. It used to be armed in SLOTS alone,
/// and that is the defect leg `p07-conc2` (2026-09-25) closed: `slot_point::count()` is MODULAR (asserted
/// < nof_slots_per_hyper_system_frame() = 10240 slots = 10.24 s at 15 kHz), so `entry.slot + 2 < slot` is false
/// for every slot in the last two of a hyperframe - and false for EVERY entry right after the counter wraps.
/// The measured waits were exact multiples of 10.24 s (30.72 s = 3.000x), each stuck block held 14 input
/// tokens, the pool went to zero, the receive thread parked in pop_blocking() for 4.998 s and the uplink went
/// silent for the whole park.
///@{

/// \brief How long a deposit may sit unclaimed before the registry commits it itself.
///
/// TIME, not slots, and that is the whole point: a deadline in TIME cannot be defeated by a modular counter,
/// and - see sweep_unclaimed() - it does not need a new deposit to be evaluated. 10 ms is five times the
/// window the slot rule uses (2 slots = 2 ms at 15 kHz) and several times the hop's own deposit -> claim
/// latency, so a block whose consumer is on its way is not reaped early.
constexpr std::chrono::milliseconds sweep_after{10};

/// The receiving chain's own progress, in slots: a block whose slot is this far behind the newest deposit has
/// had its turn - the hop and the host readers of that slot are dispatched within a slot of the symbols being
/// reported, and K = 2 is that window with room to spare.
constexpr uint64_t sweep_after_slots = 2;

/// Why the sweep is due for a block (see sweep_due()).
enum class sweep_reason {
  not_due,       ///< a consumer may still come: leave it alone
  slot_window,   ///< the receiving chain has moved past its slot (the rule that has always run)
  time_deadline  ///< nobody came within sweep_after: the Q9 trigger, and the one the slot rule cannot replace
};

/// \brief Q9: is this block past its window, i.e. must the registry claim and commit it itself?
///
/// TWO triggers, OR'd, evaluated in this order:
///
///  * the SLOT window: the newest deposit is more than `sweep_after_slots` slots past this entry's. The
///    subtraction is guarded by `entry.slot < newest_slot` FIRST, which is what makes it safe across a
///    hyperframe wrap WITHOUT the numerology (which this translation unit deliberately does not have): there,
///    an old entry's slot is LARGER than the new deposit's (10239 against 5), the guard declines, and the
///    block is caught by the time trigger instead - never by a difference that means nothing. Before a wrap
///    this is exactly the rule that has always run (measured on `p07-conc2`: 3700 late commits at concurrency
///    2, i.e. it is the trigger that fires in the healthy case).
///  * the TIME deadline: the deposit is older than `sweep_after` on the host's monotonic clock. This one needs
///    no slot at all, and it is the answer to the half of the defect the slot rule cannot fix: a slot rule only
///    fires when the slot counter moves on, and the pathology is precisely a registry that stops being called.
///
/// \note Called with handed()'s mutex HELD (it reads the entry), and with ONE reading of the clock taken by
///       the caller for the whole pass (see sweep_unclaimed()).
static sweep_reason sweep_due(const handed_entry&                   entry,
                              uint64_t                              newest_slot,
                              std::chrono::steady_clock::time_point now)
{
  if (entry.claimed || entry.produced) {
    return sweep_reason::not_due;
  }
  if ((entry.slot < newest_slot) && ((newest_slot - entry.slot) > sweep_after_slots)) {
    return sweep_reason::slot_window;
  }
  if ((entry.deposited_at != std::chrono::steady_clock::time_point{}) && ((now - entry.deposited_at) > sweep_after)) {
    return sweep_reason::time_deadline;
  }
  return sweep_reason::not_due;
}

/// \brief Q9: claims and collects every block past its window - nobody is coming for it, and its input tokens
///        have to go back.
///
/// ★ IT RUNS AT EVERY ENTRY POINT OF THE REGISTRY (deposit, take, host read), and that is not a detail. The
/// sweep used to live in the deposit path alone, and the pathology it exists for is a registry that STOPS being
/// deposited into: the unclaimed blocks hold their input tokens, the receive pool empties, the receive thread
/// parks in pop_blocking() - and with the radio parked there is no next deposit, so a sweep armed on deposits
/// can never run again. The two threads that DO keep running while the radio is parked (the hop in
/// take_released(), a host reader in claim_grid_production()) are where the way out has to be evaluated.
///
/// The block the CALLER is asking about is claimed by the caller BEFORE this runs: the sweep is for what nobody
/// came for, and it must never be the reason a consumer that DID come is served a miss.
///
/// \param[out] commit_late Receives the blocks this call claimed, in order. The CALLER commits them AFTER
///             unlocking: a commit can run a completion handler (mark_handed_produced()), which takes this
///             same mutex - so committing under the lock would deadlock on it.
/// \return How many blocks were reaped (the caller counts it - Q9-A's `reaped_by_park_blocks`).
static size_t sweep_unclaimed(handed_state& h, std::vector<id<MTLCommandBuffer>>& commit_late)
{
  const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
  size_t                                      reaped = 0;
  for (handed_entry& entry : h.entries) {
    const sweep_reason why = sweep_due(entry, h.newest_slot, now);
    if (why == sweep_reason::not_due) {
      continue;
    }
    // Claimed here so a late hop cannot adopt a buffer that is about to be committed (encoding into a
    // committed buffer is an error): it opens one of its own and reads the grid the sweep writes.
    entry.claimed = true;
    entry.swept   = true;
    // P0-7: the sweep IS the claim here, and the wait it took is the number that says how long this block sat
    // with nobody coming for it (its tokens were held for that whole time).
    if (entry.deposited_at != std::chrono::steady_clock::time_point{}) {
      entry.claim_wait_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                       now - entry.deposited_at)
                                                       .count());
      ++h.counters.claim_count;
      h.counters.claim_wait_sum_us += entry.claim_wait_us;
      h.counters.claim_wait_max_us = std::max(h.counters.claim_wait_max_us, entry.claim_wait_us);
    }
    entry.commit_issued_at = now;
    commit_late.push_back(entry.cb);
    ++h.counters.late_commits;
    ++reaped;
    if (why == sweep_reason::time_deadline) {
      // The subset the SLOT rule could not have caught: on a healthy (unwrapped) leg this stays small, and on
      // the leg that closed Q9 it is what says the time deadline is the rule that got the pool back.
      ++h.counters.late_commits_time;
    }
  }
  return reaped;
}
///@}

/// \brief P0-7: refreshes the "unclaimed and unproduced" gauge from the current entry list.
///
/// Called with handed()'s mutex HELD, at every deposit and every claim: it is the reading that says whether
/// the registry is where the chain parks (see handed_counters::unclaimed_now_max), and it costs one pass over
/// a bounded list per deposit.
static void p0_note_unclaimed_gauge(handed_state& h)
{
  const auto     now = std::chrono::steady_clock::now();
  uint64_t       sitting = 0;
  uint64_t       oldest_us = 0;
  uint64_t       oldest_slot = 0;
  for (const handed_entry& entry : h.entries) {
    if (entry.claimed || entry.produced) {
      continue;
    }
    ++sitting;
    if (entry.deposited_at != std::chrono::steady_clock::time_point{}) {
      const uint64_t age_us =
          static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now - entry.deposited_at).count());
      if (age_us > oldest_us) {
        oldest_us   = age_us;
        oldest_slot = entry.slot;
      }
    }
  }
  h.counters.unclaimed_now_max = std::max(h.counters.unclaimed_now_max, sitting);
  if (oldest_us > h.counters.unclaimed_age_max_us) {
    h.counters.unclaimed_age_max_us   = oldest_us;
    h.counters.unclaimed_age_max_slot = oldest_slot;
  }
}

/// \brief P0-7: records that a block reached completion, and keeps the slowest ones for the report.
/// \note Called with handed()'s mutex HELD.
static void p0_note_produced(handed_state& h, const handed_entry& entry)
{
  ++h.counters.produced_count;
  h.counters.produced_wait_sum_us += entry.produced_wait_us;
  h.counters.produced_wait_max_us = std::max(h.counters.produced_wait_max_us, entry.produced_wait_us);
  shared_burst::handed_counters::slow_block* worst = nullptr;
  for (shared_burst::handed_counters::slow_block& candidate : h.counters.slowest) {
    if ((worst == nullptr) || (candidate.produced_wait_us < worst->produced_wait_us)) {
      worst = &candidate;
    }
  }
  if ((worst != nullptr) && (entry.produced_wait_us > worst->produced_wait_us)) {
    worst->slot             = entry.slot;
    worst->claim_wait_us    = entry.claim_wait_us;
    worst->produced_wait_us = entry.produced_wait_us;
    worst->commit_wait_us   = entry.commit_wait_us;
    worst->registry_commit  = entry.commit_issued_at != std::chrono::steady_clock::time_point{};
    worst->claimed          = entry.claimed;
    worst->swept            = entry.swept;
    worst->used             = 1;
  }
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
      // Q9-E: how long after the GPU finished did THIS HANDLER get to run? The input tokens are released by a
      // completion handler (arm_tokens_on_complete), and so is `produced` below, so a handler dispatched late
      // holds the receive pool exactly as a buffer that RUNS late does - and P0-7 cannot tell the two apart,
      // because it timestamps the handler, not the GPU. `GPUEndTime` is the GPU's own end of this buffer, on
      // the same clock as steady_clock on Darwin (the lane probe differences the two for its queue series), so
      // `now - GPUEndTime` is the time the host spent before it looked. On `p11/p12-conc2` every slow block's
      // `commit->completion` was ~5.00 s while the lane buffers of the same slots took ~7 ms: a lag of ~5 s
      // here means the GPU was DONE and the host was late (handler dispatch), not that the queue was blocked.
      if (cb.GPUEndTime > 0.0) {
        const double now_s =
            std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const double delay_us = (now_s - cb.GPUEndTime) * 1e6;
        if (delay_us > 0.0) {
          ++h.counters.handler_lag_count;
          h.counters.handler_lag_sum_us += static_cast<uint64_t>(delay_us);
          if (static_cast<uint64_t>(delay_us) > h.counters.handler_lag_max_us) {
            h.counters.handler_lag_max_us   = static_cast<uint64_t>(delay_us);
            h.counters.handler_lag_max_slot = entry.slot;
          }
        }
      }
      entry.produced = true;
      // P0-7: deposit -> completion, the block's own end-to-end wait (its input tokens were held for this long
      // plus the slot's own time before the deposit).
      if (entry.deposited_at != std::chrono::steady_clock::time_point{}) {
        const std::chrono::steady_clock::time_point produced_at = std::chrono::steady_clock::now();
        entry.produced_wait_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(produced_at - entry.deposited_at).count());
        // Q9-B: how much of that wait came AFTER the registry had already committed the block. P0-7 said
        // "claimed in 3 ms, completed in 5.003 s" and could not say which half the 5 s was in; this is the
        // half the registry can see (the lane probe's commit -> completion table is the lane's half).
        if (entry.commit_issued_at != std::chrono::steady_clock::time_point{}) {
          entry.commit_wait_us = static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::microseconds>(produced_at - entry.commit_issued_at).count());
          ++h.counters.commit_count;
          h.counters.commit_wait_sum_us += entry.commit_wait_us;
          h.counters.commit_wait_max_us = std::max(h.counters.commit_wait_max_us, entry.commit_wait_us);
        }
        p0_note_produced(h, entry);
      }
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
  // Dev doc 6.20: the block is in the queue now, so a consumer may encode a device-side wait for its
  // generation. Marked AFTER the commit - before it, a consumer could encode its wait and be committed first.
  shared_burst::note_block_commit_issued(cb);
}

} // namespace

void shared_burst::set_drop_committer(drop_commit_fn fn)
{
  drop_committer().store(fn, std::memory_order_release);
}

void shared_burst::note_block_commit_issued(id<MTLCommandBuffer> cb)
{
  if (cb == nil) {
    return;
  }
  handed_state&               h = handed();
  std::lock_guard<std::mutex> lock(h.mutex);
  for (handed_entry& entry : h.entries) {
    if (entry.cb == cb) {
      entry.commit_issued = true;
      return;
    }
  }
  // No entry: the buffer is not a handed-over block (an ordinary burst, a front-end commit, a tool's own
  // command buffer). Not a finding - the mark only exists for the blocks a consumer can be handed.
}

shared_burst::handshake_counters shared_burst::handshake_stats()
{
  handed_state&               h = handed();
  std::lock_guard<std::mutex> lock(h.mutex);
  handshake_counters out;
  out.waits       = h.counters.handshake_waits;
  out.timeouts    = h.counters.handshake_timeouts;
  out.wait_max_us = h.counters.handshake_wait_max_us;
  return out;
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
  // ★ THE HANDLER IS ATTACHED HERE, BEFORE THE ENTRY IS PUBLISHED - and the position is load-bearing.
  //
  // Metal REQUIRES a completion handler to be installed BEFORE commit ("Completed handler provided after
  // commit call" is an assertion, not a warning); the two other sites that install one say so in as many
  // words (the gpu time probe in ocudu_metal_queue.mm, and arm_tokens_on_complete() in the DFT engine). This
  // one used to be attached AFTER the locked region, for the good reason that everything which can RUN a
  // handler has to happen outside the lock (mark_handed_produced() takes this same non-recursive mutex). But
  // the entry it belongs to is published into h.entries INSIDE that locked region, and the consumer of a
  // handed buffer runs on ANOTHER THREAD (see the note in release_block: "the adopter commits the buffer").
  // So between the publish and the attach, that thread could take the entry and commit the buffer, and the
  // attach then landed on a committed one. Measured on air, twice: gnb aborted with exactly that assertion,
  // from deposit_released <- dft_metal_engine::release_block <- ofdm_symbol_demodulator_impl::finish_symbol
  // on lower_phy_ul#0 (5.9.69; the report is in ~/Library/Logs/DiagnosticReports).
  //
  // Attaching BEFORE the publish closes that window by construction: while it is being attached the buffer is
  // not yet reachable by anyone, so nobody can commit it in between. It also stays OUTSIDE the lock, which is
  // what keeps the worst case safe rather than fatal - a buffer that had somehow been committed already would
  // run this block inline on THIS thread, where mark_handed_produced() can take the mutex; attaching inside
  // the locked region would deadlock on it instead. Registering a handler and running one are different
  // things, and it was moving both out of the lock that put this one on the wrong side of the commit.
  //
  // The record lives until the buffer COMPLETES: `no record` has to mean `nothing to wait for`, which is what
  // a host reader relies on (ensure_grid_produced()).
  [cb addCompletedHandler:^(id<MTLCommandBuffer> completed) { mark_handed_produced(completed); }];

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
        entry->commit_issued_at = std::chrono::steady_clock::now();
        commit_late.push_back(entry->cb);
      }
      released_after_unlock.push_back(entry->cb);
      entry->cb          = cb;
      entry->on_drop     = std::move(on_drop);
      entry->generation  = generation;
      entry->claimed       = false;
      entry->produced      = false;
      entry->commit_issued = false;
      // P0-7: this is a NEW block under the same key, so its timeline starts here (the replaced one never got
      // claimed - that is what `superseded` counts - and its tokens were released by its drop hook).
      entry->deposited_at     = std::chrono::steady_clock::now();
      entry->claim_wait_us    = 0;
      entry->produced_wait_us = 0;
      entry->commit_issued_at = std::chrono::steady_clock::time_point{};
      entry->commit_wait_us   = 0;
      entry->swept            = false;
      ++h.counters.handed;
      ++h.counters.superseded;
    } else {
      handed_entry entry;
      entry.grid_base   = grid_base;
      entry.slot        = slot;
      entry.cb          = cb;
      entry.on_drop     = std::move(on_drop);
      entry.generation  = generation;
      entry.deposited_at = std::chrono::steady_clock::now();
      h.entries.push_back(std::move(entry));
      ++h.counters.handed;
    }
    // Q9: this deposit is the newest slot the sweep's SLOT window is measured against.
    h.newest_slot = slot;
    // P0-7: a new deposit is the instant the "how many blocks are sitting unclaimed" gauge is worth reading
    // again (and the sweep below may claim some of them).
    p0_note_unclaimed_gauge(h);

    // ★ THE ERASE IS THE HOLE, so only PRODUCED entries are erased (5.9.62).
    //
    // "No record" has to mean "nothing to wait for" (see the note at this function's end), and the only way
    // a reader can be left with nothing while the write is still IN FLIGHT is an entry erased before its
    // block completed. This loop used to be able to do exactly that: when no produced entry existed it took
    // the oldest unproduced one, handed it to the late-commit path and erased it in the same breath. Every
    // other removal path keeps the record (supersede replaces the cb inside it, the sweep only sets
    // `claimed`), which is why this was the ONLY way the hole could open.
    //
    // It is now a SOFT bound: while nothing is produced there is nothing safe to reclaim, so the loop stops
    // and lets the completion handlers do it - the sweep commits the unclaimed ones (see below), they
    // complete, `mark_handed_produced` runs, and the next deposit reclaims them. The registry can therefore
    // sit above the bound for as long as a commit is in flight, which is bounded by the sweep window and
    // counted: `evicted_unproduced` is now always 0 by construction (kept, because it is what would say the
    // invariant broke) and `over_bound` says the registry is above the bound with nothing to reclaim - the
    // pressure reading that replaces it.
    while (h.entries.size() > shared_burst::handed_capacity()) {
      size_t victim = SIZE_MAX;
      for (size_t i = 0; i != h.entries.size(); ++i) {
        if (h.entries[i].produced) {
          victim = i;
          break;
        }
      }
      if (victim == SIZE_MAX) {
        ++h.counters.over_bound;
        break;
      }
      if (h.entries[victim].on_drop) {
        dropped.push_back(std::move(h.entries[victim].on_drop));
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
    // The sweep is what reaps those blocks, and since Q9 (§6.10) it is armed on TWO triggers - the receiving
    // chain's own progress in slots, and a 10 ms deadline on the host's monotonic clock - and evaluated at
    // EVERY entry point of the registry rather than on deposits alone (see sweep_unclaimed()).
    (void)sweep_unclaimed(h, commit_late);
  }
  // NOTE: the completion handler was attached at the TOP of this function, BEFORE the entry was published -
  // it cannot be attached here, because by now another thread may already have claimed and committed the
  // buffer (see the note there).

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
  handed_state&                     h    = handed();
  id<MTLCommandBuffer>              taken = nil;
  std::vector<id<MTLCommandBuffer>> commit_late;
  {
    std::lock_guard<std::mutex> lock(h.mutex);
    handed_entry*               entry = find_handed(h, grid_base, slot);
    if ((entry != nullptr) && !entry->claimed) {
      entry->claimed = true;
      ++h.counters.taken;
      // P0-7: how long this block waited for its consumer. A large value here is the hand-over being late, not
      // the GPU being slow - and it is the number the sweep's deadline is measured against.
      if (entry->deposited_at != std::chrono::steady_clock::time_point{}) {
        entry->claim_wait_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                         std::chrono::steady_clock::now() - entry->deposited_at)
                                                         .count());
        ++h.counters.claim_count;
        h.counters.claim_wait_sum_us += entry->claim_wait_us;
        h.counters.claim_wait_max_us = std::max(h.counters.claim_wait_max_us, entry->claim_wait_us);
      }
      taken = entry->cb;
    }
    // ★ Q9: this is one of the two entry points that KEEP RUNNING while the radio is parked, so it is where
    // the sweep has to be evaluated (see sweep_unclaimed()). The block this caller came for was claimed ABOVE,
    // so the sweep can only reap what nobody asked for - a consumer that did come is never served a miss by it.
    (void)sweep_unclaimed(h, commit_late);
  }
  for (id<MTLCommandBuffer> late : commit_late) {
    commit_dropped(late);
  }
  return taken;
}

/// \brief The production promise of (\p grid_base, \p slot): commits it when nobody claimed it, and returns
///        the generation that will mark its completion (0 when there is no record, or none was armed).
///
/// Shared by the two consumers that need it, which differ only in HOW they wait: a host reader blocks on
/// the event (ensure_grid_produced()), a device consumer encodes the wait into its own command buffer
/// (grid_production_generation()). The fallback - a consumer committing a block no hop claimed - is the
/// same debt in both cases, and so is the counter that records it.
///
/// \param[out] to_commit Receives the blocks this call claimed on the caller's behalf - the requested one when
///             nobody had claimed it, plus everything the sweep reaped - in that order. The caller commits
///             them OUTSIDE the lock (a commit can run completion handlers, which take this same lock).
static uint64_t claim_grid_production(handed_state&                      h,
                                      const void*                        grid_base,
                                      uint64_t                           slot,
                                      std::vector<id<MTLCommandBuffer>>& to_commit,
                                      bool*                              must_wait_for_commit = nullptr)
{
  uint64_t      generation = 0;
  handed_entry* entry      = find_handed(h, grid_base, slot);
  if (must_wait_for_commit != nullptr) {
    *must_wait_for_commit = false;
  }
  if (entry == nullptr) {
    // No record at all: either nothing was ever handed over for this (storage, slot) - no hand-over in this
    // build or run - or it was produced long enough ago to be evicted. Counted, because a LATE reader that
    // cannot wait is exactly the case the key was introduced for.
    ++h.counters.grid_not_found;
  } else {
    generation = entry->generation;
    // ★ DEV DOC 6.20: the generation may only be encoded as a DEVICE-side wait once the command buffer that
    // carries the signal has been COMMITTED. Metal holds the whole queue for 5.00 s on an unsatisfied wait and
    // then drops it, so a consumer committed before its producer costs 5 s AND loses the ordering. When the
    // block is claimed but its commit has not been issued yet (the sweep claims under its lock and commits
    // after unlocking; another lane may still be holding the block), the caller waits for that commit - see
    // wait_for_block_commit() - instead of trusting the order.
    if (must_wait_for_commit != nullptr) {
      *must_wait_for_commit = (generation != 0) && entry->claimed && !entry->produced && !entry->commit_issued;
    }
    if (!entry->claimed && !entry->produced) {
      // Nobody will ever commit this one (a slot no hop ran for), and a grid nobody produces is a grid the
      // caller is about to read as garbage: the fallback a hand-over owes. Claimed and collected BEFORE the
      // sweep below, for the same reason as in take_released(): the block the caller asked about is served
      // first, and the sweep is only for what nobody came for.
      entry->claimed          = true;
      entry->commit_issued_at = std::chrono::steady_clock::now();
      to_commit.push_back(entry->cb);
      ++h.counters.fallback_commits;
    }
  }
  // ★ Q9: the host reader is the other entry point that keeps running while the radio is parked - the PUCCH
  // and the SRS read their grids on the HOST thread, which the receive stall never blocks. This is where the
  // pool gets its buffers back when no deposit is coming (see sweep_unclaimed()).
  (void)sweep_unclaimed(h, to_commit);
  return generation;
}

bool shared_burst::ensure_grid_produced(const void* grid_base, uint64_t slot)
{
  if (grid_base == nullptr) {
    return true;
  }
  std::vector<id<MTLCommandBuffer>> to_commit;
  uint64_t                          generation = 0;
  {
    handed_state&               h = handed();
    std::lock_guard<std::mutex> lock(h.mutex);
    generation = claim_grid_production(h, grid_base, slot, to_commit);
  }
  for (id<MTLCommandBuffer> cb : to_commit) {
    // A consumer had to commit it (fallback), or the sweep did: both are counted apart from each other, but
    // the action is the same commit - the first says a host reader found the block nobody claimed, the second
    // that nobody came at all.
    commit_dropped(cb);
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

/// \brief Dev doc 6.20: waits (bounded) for the command buffer carrying \p grid_base's signal to be committed.
///
/// WHY IT IS NEEDED AT ALL. `grid_production_generation()` hands a consumer the generation of a block that
/// ANOTHER party claimed, and the consumer encodes a device-side wait for it. That wait is only safe - and
/// only cheap - when the carrier is already in the queue: Metal releases an unsatisfied wait after 5.00 s and
/// holds the whole queue until then (measured, wip/metal_wait_timeout_probe.mm), and after the bound the wait
/// is dropped, so the fence stops being a fence.
///
/// HOW LONG IT TAKES. The window it closes is the registry's own "claim under the lock, commit after
/// unlocking" gap (microseconds) or another lane's hop holding the block until it commits (up to that hop's
/// span). The wait is therefore bounded by \p bound and POLLED: the committer needs the registry's mutex to
/// publish the mark, so sleeping while holding it would deadlock.
///
/// \return True when the carrier is committed (or already produced, which the event has reached by then).
static bool wait_for_block_commit(const void* grid_base, uint64_t slot, std::chrono::microseconds bound)
{
  handed_state&     h        = handed();
  const auto        deadline = std::chrono::steady_clock::now() + bound;
  const auto        started  = std::chrono::steady_clock::now();
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(h.mutex);
      handed_entry*               entry = find_handed(h, grid_base, slot);
      if ((entry == nullptr) || entry->commit_issued || entry->produced) {
        const uint64_t waited_us =
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                                        started)
                                      .count());
        ++h.counters.handshake_waits;
        h.counters.handshake_wait_max_us = std::max(h.counters.handshake_wait_max_us, waited_us);
        return true;
      }
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(20));
  }
}

uint64_t shared_burst::grid_production_generation(const void* grid_base, uint64_t slot)
{
  if (grid_base == nullptr) {
    return 0;
  }
  // How long a consumer may wait for the carrier's commit before it gives up on the DEVICE ordering. The
  // window it closes is microseconds wide (the registry's claim->commit gap); a claiming hop's own span is
  // the long tail, and the waiter would have to wait for that block's COMPLETION anyway.
  constexpr std::chrono::microseconds commit_handshake_bound{2000};

  std::vector<id<MTLCommandBuffer>> to_commit;
  uint64_t                          generation           = 0;
  bool                              must_wait_for_commit = false;
  {
    handed_state&               h = handed();
    std::lock_guard<std::mutex> lock(h.mutex);
    generation = claim_grid_production(h, grid_base, slot, to_commit, &must_wait_for_commit);
  }
  for (id<MTLCommandBuffer> cb : to_commit) {
    commit_dropped(cb);
  }
  if (must_wait_for_commit && !wait_for_block_commit(grid_base, slot, commit_handshake_bound)) {
    // ★ The carrier's commit did not come: DO NOT hand the generation out (a device-side wait for it can cost
    // 5 s and is dropped after that), and order the consumer on the HOST instead - the pre-5.9.23 behaviour,
    // bounded at 200 ms and counted, and correct: the grid the caller is about to read really is produced by
    // that block. This must stay at 0; a non-zero value is a leg whose consumers had to be ordered on the host.
    {
      handed_state&               h = handed();
      std::lock_guard<std::mutex> lock(h.mutex);
      ++h.counters.handshake_timeouts;
    }
    // Order the consumer on the HOST instead - the pre-5.9.23 behaviour, bounded at 200 ms and counted by
    // ready_timeouts. It is the correct wait (the grid the caller is about to read really is produced by that
    // block); what it costs is a blocked consumer, which is strictly better than a queue held for 5 s.
    (void)ensure_grid_produced(grid_base, slot);
    return 0;
  }
  // The caller may also learn here that the block was already produced: the generation it gets then names a
  // value the event has reached, and encoding the wait on it is a satisfied wait rather than a mistake.
  return generation;
}

void shared_burst::reap_unclaimed_now(handover_reap_hook::reap_reason why)
{
  std::vector<id<MTLCommandBuffer>> commit_late;
  {
    handed_state&               h = handed();
    std::lock_guard<std::mutex> lock(h.mutex);
    // Counted BEFORE the sweep, so a leg can tell "a dry pool asked, and there was nothing to reap" (the
    // holder is then a CLAIMED block, which the sweep must not touch) from "a dry pool never asked at all" -
    // and, since dev doc 6.34, the same pair for the ORDINARY take, which is the entry point a quiet window
    // needs (see handover_reap_hook).
    const uint64_t reaped  = sweep_unclaimed(h, commit_late);
    const bool     by_take = (why == handover_reap_hook::reap_reason::take);
    if (by_take) {
      ++h.counters.reaped_by_take_events;
      h.counters.reaped_by_take_blocks += reaped;
    } else {
      ++h.counters.reaped_by_park_events;
      h.counters.reaped_by_park_blocks += reaped;
    }
  }
  for (id<MTLCommandBuffer> late : commit_late) {
    commit_dropped(late);
  }
}

void shared_burst::set_stage_wait(uint64_t generation)
{
  state().stage_wait = generation;
}

bool shared_burst::stage_wait_pending()
{
  return state().stage_wait != 0;
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
