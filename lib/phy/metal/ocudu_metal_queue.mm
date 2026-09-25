// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu/phy/phy_pipeline_contract.h"
#include "ocudu/phy/phy_pipeline_report.h"
#include "ocudu_metal_queue.h"

#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/support/macos_compat.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <vector>

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
  /// \name Q9-D: the fence waits that are open right now, and what became of them.
  ///
  /// A wait is open from the moment it is encoded until a signal REACHES its generation (Metal's shared-event
  /// wait fires on `value >= generation`). It is therefore resolved by the NEXT signal at or above it, which is
  /// what makes the duration measurable from the host: the signal sites are host calls (the signal is encoded
  /// immediately before the commit that will publish it), and their distance from the wait is the number the
  /// report prints. Bounded, because a wait whose signaller never comes would otherwise grow this forever.
  ///@{
  std::mutex            fence_mutex;
  struct fence_wait {
    uint64_t                           generation = 0;
    shared_queue::fence_kind           kind       = shared_queue::fence_kind::stage;
    uint64_t                           slot       = 0;
    bool                               has_slot   = false;
    std::chrono::steady_clock::time_point wait_at{};
  };
  std::vector<fence_wait> fence_pending;
  std::atomic<uint64_t>   fence_waits{0};
  /// Waits whose signaller had already been encoded: satisfied at once, the shape that can never block.
  std::atomic<uint64_t>   fence_waits_before{0};
  std::atomic<uint64_t>   fence_waits_after{0};
  std::atomic<uint64_t>   fence_wait_after_max_us{0};
  /// Slot and kind of the longest wait whose signaller came after it (`fence_wait_after_worst_kind`).
  std::atomic<uint64_t>   fence_wait_after_worst_slot{0};
  std::atomic<unsigned>   fence_wait_after_worst_kind{0};
  ///@}

  /// Q9-C: which generation the waits named (see shared_queue::note_stage_fence_wait).
  std::atomic<uint64_t>    stage_fence_own_waits{0};
  std::atomic<uint64_t>    stage_fence_newest_waits{0};
  std::atomic<uint64_t>    stage_fence_cross_lane{0};
  std::atomic<uint64_t>    stage_fence_skipped_waits{0};

  /// \name Q9-F: the COMMIT order of the device-side fences' two ends (dev doc 6.19).
  ///
  /// Q9-D resolves a wait against the moment its signaller's generation was HANDED OUT. That is not when the
  /// signal is submitted: the signal rides a command buffer, and that buffer may be committed by another
  /// thread microseconds later (the registry's sweep commits a claimed hand-over block after dropping its
  /// lock). On a queue whose starts are ordered by submission, a waiter committed before its signaller is a
  /// waiter whose signaller is behind it - the shape that cannot resolve until a later signal arrives.
  ///
  /// The instrument therefore takes a TICKET immediately before every commit that can carry a fence, keeps
  /// the fences armed on each command buffer until that commit resolves them, and compares:
  ///  * `order_max_signal_generation` - the highest generation whose carrier has been COMMITTED. A wait for a
  ///    value at or below it is safe by construction (its signaller is already ahead of it in the queue);
  ///  * otherwise the wait is an inversion, remembered until a signal reaches it so its DURATION (waiter's
  ///    commit -> signaller's commit) and the two buffers' queues can be reported.
  ///@{
  std::atomic<uint64_t> commit_ticket{0};
  /// The highest generation whose carrier has been COMMITTED. An event's value only grows, so a wait for a
  /// value at or below this can be satisfied by a command buffer that is already ahead of the waiter's.
  std::atomic<uint64_t> order_max_signal_generation{0};
  std::atomic<uint64_t> order_commits{0};
  std::atomic<uint64_t> order_waits{0};
  std::atomic<uint64_t> order_waiter_first{0};
  std::atomic<uint64_t> order_waiter_first_same_queue{0};
  std::atomic<uint64_t> order_waiter_first_cross_queue{0};
  std::atomic<uint64_t> order_worst_us{0};
  std::atomic<uint64_t> order_worst_slot{0};
  std::atomic<unsigned> order_worst_kind{0};
  /// Fences armed on a command buffer that has not been committed yet, keyed by the buffer.
  struct order_pending_entry {
    std::vector<uint64_t> signals; ///< generations this buffer will signal (grid / stage / corr)
    std::vector<uint64_t> waits;   ///< generations this buffer waits for
    std::vector<shared_queue::fence_kind> wait_kinds;
    std::vector<uint64_t> wait_slots;
    std::vector<bool>     wait_has_slot;
    std::vector<std::chrono::steady_clock::time_point> wait_at;
  };
  std::mutex                                   order_mutex;
  std::unordered_map<const void*, order_pending_entry> order_pending;
  /// Inversions waiting for their signaller, so the duration can be measured when it commits. Bounded: a
  /// signaller that never commits must not grow this forever (its count is what `unresolved` reports).
  struct order_open_wait {
    uint64_t                              generation = 0;
    shared_queue::fence_kind              kind       = shared_queue::fence_kind::stage;
    uint64_t                              slot       = 0;
    unsigned                              queue      = 0; ///< 0 = front-end, 1 = back-end, 2 = unknown
    std::chrono::steady_clock::time_point wait_at{};
  };
  std::vector<order_open_wait> order_open;
  /// Inversions the bound did not let the list keep: counted, because a reading that silently drops what it
  /// cannot hold is how "0 inversions" gets believed (see nof_commit_order_unresolved()).
  std::atomic<uint64_t>        order_open_overflow{0};
  std::atomic<uint64_t>        order_kind_waits[static_cast<size_t>(shared_queue::fence_kind::count)]   = {};
  std::atomic<uint64_t>        order_kind_inversions[static_cast<size_t>(shared_queue::fence_kind::count)] = {};
  ///@}

  /// \name Q9-F3: one record per command buffer armed with the GPU-time probe (see shared_queue::arm_gpu_time).
  ///
  /// The union of these windows, per queue, is what the device actually executed; the holes in it are the
  /// intervals in which the queue had NOTHING running - and the label/slot of the buffer that starts right
  /// after a hole is the answer to "what was waiting, and for how long". `commit_ns` is the host time at
  /// which the probe was armed (i.e. immediately before the commit), so `commit -> start` is the same
  /// quantity the lane probe's Q9-B table prints per command buffer - here for every commit, including the
  /// ones no engine registers anywhere.
  ///@{
  struct occupancy_record {
    uint64_t    start_ns  = 0;
    uint64_t    end_ns    = 0;
    uint64_t    commit_ns = 0;
    uint64_t    slot      = 0;
    bool        has_slot  = false;
    const char* label     = nullptr;
  };
  std::mutex                occupancy_mutex;
  std::vector<occupancy_record> occupancy;
  std::atomic<uint64_t>     occupancy_dropped{0};
  std::atomic<uint64_t>     occupancy_largest_idle_us{0};
  ///@}

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

namespace {
/// The waiting hop's slot, when the calling thread knows it (Q9-D): the lane clock lives with the Metal lane
/// probe and the queue does not include it, so a weak accessor is installed by the probe at start-up. Missing
/// accessor (the unit tests, the replay tool) means "no slot", which the report prints as 0.
///
/// Declared at the top of the translation unit's anonymous namespace because the Q9-F/Q9-F3 instruments stamp
/// their records with the slot as well (see arm_gpu_time and note_commit_order), and those run on the same
/// threads that name it.
bool (*lane_has_slot_fn)()  = nullptr;
uint64_t (*lane_slot_fn)()  = nullptr;

bool lane_has_slot()
{
  return (lane_has_slot_fn != nullptr) && lane_has_slot_fn();
}

uint64_t lane_slot()
{
  return (lane_slot_fn != nullptr) ? lane_slot_fn() : 0;
}

/// The name of a fence kind, for the reports (Q9-D and Q9-F read it the same way).
const char* fence_kind_name(shared_queue::fence_kind kind)
{
  switch (kind) {
    case shared_queue::fence_kind::stage:
      return "stage";
    case shared_queue::fence_kind::correlation:
      return "corr";
    case shared_queue::fence_kind::grid:
      return "grid";
    case shared_queue::fence_kind::count:
      break;
  }
  return "?";
}
} // namespace

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
               "[metal_stats] lane fence signals=%llu waits=%llu skipped=%llu generation=%llu "
               "own=%llu newest=%llu cross_lane=%llu (Q9-C: own = the wait named THIS hop's own estimator "
               "generation; cross_lane = the global newest differed, i.e. how often the old rule would have "
               "waited for another lane's)\n",
               static_cast<unsigned long long>(s.stage_fence_signals.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.stage_fence_waits.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.stage_fence_skipped_waits.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.stage_fence_generation.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.stage_fence_own_waits.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.stage_fence_newest_waits.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.stage_fence_cross_lane.load(std::memory_order_relaxed)));
  // Q9-D: the device-side fence waits that named a signaller which had NOT been handed out yet, and how long
  // they then lasted. A wait is safe when its signaller came first (the stage fence's intended shape, the
  // correlation fence, a grid producer already committed); when it did not, the signaller's command buffer may
  // reach the queue after the waiter's, and on a serial queue that ordering cannot be satisfied until the
  // signaller runs - while the signaller cannot run until the waiter does. `max` is the reading that says
  // whether a leg's stall sits in a fence at all, and `worst` names which one and for which slot.
  {
    uint64_t    worst_slot = 0;
    const char* worst_kind = shared_queue::fence_wait_after_worst_kind(worst_slot);
    const double worst_ms  = static_cast<double>(s.fence_wait_after_max_us.load(std::memory_order_relaxed)) / 1000.0;
    std::fprintf(stderr,
                 "[metal_stats] fence order (Q9-D): waits=%llu signaller-first=%llu signaller-after=%llu "
                 "max=%.1fms worst kind=%s slot=%llu%s\n",
                 static_cast<unsigned long long>(s.fence_waits.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(s.fence_waits_before.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(s.fence_waits_after.load(std::memory_order_relaxed)),
                 worst_ms,
                 worst_kind,
                 static_cast<unsigned long long>(worst_slot),
                 (s.fence_pending.empty()) ? "" : " (waits still open at exit: their signaller never came)");
  }
  // Q9-F: the same questions as Q9-D, asked of the COMMIT order instead of the moment the generation was
  // handed out - the blind spot Q9-D documented (a signal handed out early can still be SUBMITTED late, and
  // the registry's sweep commits a claimed block from another thread after dropping its lock).
  {
    shared_queue::fence_kind worst_kind = shared_queue::fence_kind::stage;
    uint64_t                 worst_slot = 0;
    const uint64_t           worst_us = shared_queue::commit_order_worst_us(worst_kind, worst_slot);
    const uint64_t unresolved = shared_queue::nof_commit_order_unresolved();
    std::fprintf(stderr,
                 "[metal_stats] commit order (Q9-F): commits=%llu waits=%llu waiter-committed-first=%llu "
                 "(same-queue=%llu cross-queue=%llu) max=%.1fms worst kind=%s slot=%llu; per kind: "
                 "stage %llu/%llu, corr %llu/%llu, grid %llu/%llu (inversions/waits)%s\n",
                 static_cast<unsigned long long>(s.order_commits.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(s.order_waits.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(s.order_waiter_first.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(s.order_waiter_first_same_queue.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(s.order_waiter_first_cross_queue.load(std::memory_order_relaxed)),
                 static_cast<double>(worst_us) / 1000.0,
                 fence_kind_name(worst_kind),
                 static_cast<unsigned long long>(worst_slot),
                 static_cast<unsigned long long>(s.order_kind_inversions[0].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(s.order_kind_waits[0].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(s.order_kind_inversions[1].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(s.order_kind_waits[1].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(s.order_kind_inversions[2].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(s.order_kind_waits[2].load(std::memory_order_relaxed)),
                 (unresolved == 0)
                     ? ""
                     : " (waits still open at exit: no signal at or above them was ever committed, or the "
                       "open list overflowed - see its comment)");
  }
  // Q9-F3: the QUEUE-occupancy timeline (see arm_gpu_time() and the state's records). Q9-F says whether a
  // waiter is ahead of its signaller; this says what the device was doing while it waited. The union of the
  // recorded GPU windows, per queue, is what the device actually executed: the HOLES in it are the intervals
  // in which that queue had nothing running at all, and the label/slot of the buffer that starts right after
  // a hole is what was waiting for it.
  //
  // NOTE the probe this reads is opt-in (OCUDU_METAL_GPU_TIME=1): without it there are no records and this
  // line reports 0 - which is "not measured", not "no hole".
  {
    std::vector<shared_queue_state::occupancy_record> records;
    uint64_t                                         dropped = 0;
    {
      std::lock_guard<std::mutex> lock(s.occupancy_mutex);
      records = s.occupancy;
      dropped = s.occupancy_dropped.load(std::memory_order_relaxed);
    }
    if (records.empty()) {
      std::fprintf(stderr,
                   "[metal_stats] queue occupancy (Q9-F3): no GPU-time records - the probe is off "
                   "(OCUDU_METAL_GPU_TIME=1 turns it on), so nothing here says whether the queue was idle\n");
    } else {
      std::sort(records.begin(), records.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.start_ns < rhs.start_ns;
      });
      // The union: walk the windows in start order, keeping the frontier the device has covered.
      struct hole {
        uint64_t    start_ns = 0;
        uint64_t    us       = 0;
        const char* label    = nullptr;
        uint64_t    slot     = 0;
        bool        has_slot = false;
      };
      std::vector<hole> holes;
      uint64_t          frontier   = 0;
      uint64_t          busy_union = 0;
      // A hole worth reporting is longer than the ordinary spacing between two submissions (tens of us);
      // 100 us is the same order the lane probe's D-series uses for "this is not jitter".
      constexpr uint64_t hole_threshold_ns = 100000;
      for (const shared_queue_state::occupancy_record& r : records) {
        if (r.start_ns > frontier) {
          const uint64_t gap_ns = r.start_ns - frontier;
          if ((frontier != 0) && (gap_ns > hole_threshold_ns)) {
            hole h;
            h.start_ns = frontier;
            h.us       = gap_ns / 1000;
            h.label    = r.label;
            h.slot     = r.slot;
            h.has_slot = r.has_slot;
            holes.push_back(h);
            if (h.us > s.occupancy_largest_idle_us.load(std::memory_order_relaxed)) {
              s.occupancy_largest_idle_us.store(h.us, std::memory_order_relaxed);
            }
          }
          frontier = r.end_ns;
          busy_union += r.end_ns - r.start_ns;
        } else if (r.end_ns > frontier) {
          busy_union += r.end_ns - frontier;
          frontier = r.end_ns;
        }
      }
      const uint64_t window_ns = (frontier > records.front().start_ns) ? (frontier - records.front().start_ns) : 0;
      // The slowest commits by commit -> GPU start: the same reading as the lane probe's Q9-B table, but for
      // EVERY commit - including the ones no engine registers anywhere (the registry's late hand-over commits,
      // which is where a stall's waitee can hide).
      std::vector<size_t> slowest(records.size());
      for (size_t i = 0; i != records.size(); ++i) {
        slowest[i] = i;
      }
      const size_t nof_slow = std::min<size_t>(8, slowest.size());
      std::partial_sort(slowest.begin(),
                        slowest.begin() + static_cast<std::ptrdiff_t>(nof_slow),
                        slowest.end(),
                        [&records](size_t lhs, size_t rhs) {
                          const uint64_t l = (records[lhs].start_ns > records[lhs].commit_ns)
                                                 ? (records[lhs].start_ns - records[lhs].commit_ns)
                                                 : 0;
                          const uint64_t r = (records[rhs].start_ns > records[rhs].commit_ns)
                                                 ? (records[rhs].start_ns - records[rhs].commit_ns)
                                                 : 0;
                          return l > r;
                        });
      std::fprintf(stderr,
                   "[metal_stats] queue occupancy (Q9-F3): commits=%zu busy(union)=%.1fus window=%.1fus "
                   "holes>100us=%zu largest=%.1fms%s\n",
                   records.size(),
                   static_cast<double>(busy_union) / 1e3,
                   static_cast<double>(window_ns) / 1e3,
                   holes.size(),
                   static_cast<double>(shared_queue::occupancy_largest_idle_us()) / 1000.0,
                   (dropped == 0) ? "" : " (records DROPPED over the bound)");
      std::fprintf(stderr,
                   "[metal_stats] queue occupancy (Q9-F3) slowest commits, by commit -> GPU start "
                   "(label = what the buffer carries):\n");
      for (size_t i = 0; i != nof_slow; ++i) {
        const shared_queue_state::occupancy_record& r = records[slowest[i]];
        if (r.commit_ns == 0) {
          continue;
        }
        const double to_start_us = (r.start_ns > r.commit_ns) ? (static_cast<double>(r.start_ns - r.commit_ns) / 1e3) : 0.0;
        std::fprintf(stderr,
                     "[metal_stats]   label=%-12s slot=%llu commit->start=%.1fus start->end=%.1fus\n",
                     (r.label != nullptr) ? r.label : "?",
                     static_cast<unsigned long long>(r.has_slot ? r.slot : 0),
                     to_start_us,
                     static_cast<double>(r.end_ns - r.start_ns) / 1e3);
      }
      // The largest holes, worst first, each named by the commit that waited through it.
      std::sort(holes.begin(), holes.end(), [](const hole& lhs, const hole& rhs) { return lhs.us > rhs.us; });
      const size_t nof_holes = std::min<size_t>(4, holes.size());
      if (nof_holes == 0) {
        std::fprintf(stderr,
                     "[metal_stats] queue occupancy (Q9-F3): no hole over 100us in the window - the device was "
                     "continuously executing something (a stalled waiter is then behind a RUNNING buffer)\n");
      }
      for (size_t i = 0; i != nof_holes; ++i) {
        std::fprintf(stderr,
                     "[metal_stats]   hole %.1fms -> next label=%s slot=%llu (nothing was executing on any "
                     "probed queue for that long)\n",
                     static_cast<double>(holes[i].us) / 1000.0,
                     (holes[i].label != nullptr) ? holes[i].label : "?",
                     static_cast<unsigned long long>(holes[i].has_slot ? holes[i].slot : 0));
      }
    }
  }
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
  // ... and it joins the on-demand dump (dev doc 6.24): a parked receive thread prints every P0 reading while
  // the stall is happening, which is the one moment an atexit report can never describe.
  register_p0_report(shared_queue_stats_report);
  return true;
}();
#endif

shared_queue_state& state()
{
  // Never destroyed on purpose, like every other registry a report reads (see ocudu_metal_lane_probe.mm's
  // stats() and ocudu_metal_burst.mm's handed()): the [metal_stats] report runs from an atexit handler that
  // this translation unit registers at STATIC-INITIALIZATION time, while this object is constructed on first
  // USE - i.e. later - so the handler runs AFTER the object has been destroyed and every mutex in it is gone.
  // Measured (2026-09-25): `libc++abi: terminating due to uncaught exception ... mutex lock failed: Invalid
  // argument`, raised by the Q9-F3 report - the first thing in here that locks during a report.
  static shared_queue_state* s = new shared_queue_state();
  return *s;
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


void shared_queue::install_lane_slot_accessors(bool (*has_slot)(), uint64_t (*slot)())
{
  lane_has_slot_fn = has_slot;
  lane_slot_fn     = slot;
}

void shared_queue::arm_gpu_time(id<MTLCommandBuffer> command_buffer, queue_kind kind, const char* label, uint64_t slot)
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
    (void)label;
    (void)slot;
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
  // Q9-F3: the same handler leaves one record per command buffer (see the state's note), so the report can
  // compute the union of the queue's windows and the holes in it. \p label and \p slot travel BY VALUE into
  // the block: a string literal outlives the process, and the slot is copied because the caller's own table
  // entry may be gone by the time the GPU is done.
  const uint64_t       resolved_slot = (slot != no_slot) ? slot : (lane_has_slot() ? lane_slot() : 0);
  const bool           has_slot      = (slot != no_slot) || lane_has_slot();
  const double         commit_s =
      std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
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
    shared_queue_state& st = state();
    constexpr size_t    max_records = 1u << 21; // ~2M command buffers: a leg records ~1e5
    std::lock_guard<std::mutex> lock(st.occupancy_mutex);
    if (st.occupancy.size() < max_records) {
      shared_queue_state::occupancy_record r;
      r.start_ns  = start_ns;
      r.end_ns    = end_ns;
      r.commit_ns = static_cast<uint64_t>(commit_s * 1e9);
      r.slot      = resolved_slot;
      r.has_slot  = has_slot;
      r.label     = (label != nullptr) ? label : "?";
      st.occupancy.push_back(r);
    } else {
      st.occupancy_dropped.fetch_add(1, std::memory_order_relaxed);
    }
  }];
#else
  (void)command_buffer;
  (void)kind;
  (void)label;
  (void)slot;
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
  note_fence_signal(generation, fence_kind::grid, command_buffer);
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
  note_fence_wait(generation, fence_kind::grid, command_buffer);
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
  // Q9-D: the fence this signal belongs to cannot be told apart here (the stage and the correlation fences
  // share this event and this generation counter), so it is recorded as `stage` and the report says so.
  note_fence_signal(generation, fence_kind::stage, command_buffer);
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
  note_fence_wait(generation, fence_kind::stage, command_buffer);
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

void shared_queue::note_fence_signal(uint64_t generation, fence_kind kind, id<MTLCommandBuffer> command_buffer)
{
  (void)kind;
  if (generation == 0) {
    return;
  }
  shared_queue_state& s = state();
  // Q9-F: the signal is a fact for the COMMIT order only once the buffer carrying it is committed, so the
  // pending entry is what note_commit_order() resolves. Recorded before the Q9-D bookkeeping below, which may
  // take the same mutex: both live under `order_mutex`/`fence_mutex` respectively and are taken one at a time.
  if (command_buffer != nil) {
    std::lock_guard<std::mutex> order_lock(s.order_mutex);
    s.order_pending[(__bridge const void*)command_buffer].signals.push_back(generation);
    // A command buffer that is never committed (a deposit nobody claims and the registry drops without
    // committing) would otherwise leave its entry behind forever. The bound is generous next to the handful
    // of buffers that are armed-but-uncommitted at any instant (measured: 1-2).
    while (s.order_pending.size() > 512) {
      s.order_pending.erase(s.order_pending.begin());
    }
  }
  std::lock_guard<std::mutex> lock(s.fence_mutex);
  // Resolve every open wait this signal reaches. An event's value only grows, so a signal at G satisfies every
  // wait for a value <= G: resolving them here (rather than at the true GPU instant) keeps the reading on ONE
  // clock and is exact for the ORDER question, which is the one that matters.
  const auto now = std::chrono::steady_clock::now();
  for (auto it = s.fence_pending.begin(); it != s.fence_pending.end();) {
    if (it->generation > generation) {
      ++it;
      continue;
    }
    // The signaller was encoded AFTER this wait: the waiter named a signal that had not been handed out yet,
    // so the two command buffers may reach the queue in that order - see the header.
    const uint64_t waited_us =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now - it->wait_at).count());
    s.fence_waits_after.fetch_add(1, std::memory_order_relaxed);
    if (waited_us > s.fence_wait_after_max_us.load(std::memory_order_relaxed)) {
      s.fence_wait_after_max_us.store(waited_us, std::memory_order_relaxed);
      s.fence_wait_after_worst_slot.store(it->slot, std::memory_order_relaxed);
      s.fence_wait_after_worst_kind.store(static_cast<unsigned>(it->kind), std::memory_order_relaxed);
    }
    it = s.fence_pending.erase(it);
  }
  // A wait whose signaller comes first never enters this list: it is resolved at the instant it is encoded
  // (see note_fence_wait), which is the SAFE shape and is counted as such.
  while (s.fence_pending.size() > 256) {
    s.fence_pending.erase(s.fence_pending.begin());
  }
}

void shared_queue::note_fence_wait(uint64_t generation, fence_kind kind, id<MTLCommandBuffer> command_buffer)
{
  if (generation == 0) {
    return;
  }
  shared_queue_state& s = state();
  // Q9-F: the waiter's side of the record, kept on the buffer it was encoded into until that buffer commits -
  // which is the instant the question "was the signaller ahead of me?" can be answered at all.
  if (command_buffer != nil) {
    std::lock_guard<std::mutex> order_lock(s.order_mutex);
    shared_queue_state::order_pending_entry& pending = s.order_pending[(__bridge const void*)command_buffer];
    pending.waits.push_back(generation);
    pending.wait_kinds.push_back(kind);
    pending.wait_slots.push_back(lane_has_slot() ? lane_slot() : 0);
    pending.wait_has_slot.push_back(lane_has_slot());
    pending.wait_at.push_back(std::chrono::steady_clock::now());
    while (s.order_pending.size() > 512) {
      s.order_pending.erase(s.order_pending.begin());
    }
  }
  std::lock_guard<std::mutex> lock(s.fence_mutex);
  s.fence_waits.fetch_add(1, std::memory_order_relaxed);
  // Was this generation handed out already? Generations are handed out AT THE MOMENT the signal is encoded
  // (immediately before the signaller's commit), so a generation at or below the current counter means the
  // wait is satisfied the instant it is encoded: it can never block the queue, and it is counted as the SAFE
  // shape instead of being remembered. Only a wait for a generation that does not exist yet is remembered.
  const uint64_t highest_signalled = (kind == fence_kind::grid)
                                         ? s.grid_generation.load(std::memory_order_acquire)
                                         : s.stage_fence_generation.load(std::memory_order_acquire);
  if (generation <= highest_signalled) {
    s.fence_waits_before.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  shared_queue_state::fence_wait open;
  open.generation = generation;
  open.kind       = kind;
  open.slot       = lane_has_slot() ? lane_slot() : 0;
  open.has_slot   = lane_has_slot();
  open.wait_at    = std::chrono::steady_clock::now();
  s.fence_pending.push_back(open);
}

uint64_t shared_queue::nof_fence_waits()
{
  return state().fence_waits.load(std::memory_order_relaxed);
}

uint64_t shared_queue::nof_fence_waits_after_signaller()
{
  return state().fence_waits_after.load(std::memory_order_relaxed);
}

uint64_t shared_queue::nof_fence_waits_before_signaller()
{
  return state().fence_waits_before.load(std::memory_order_relaxed);
}

uint64_t shared_queue::fence_wait_after_max_us()
{
  return state().fence_wait_after_max_us.load(std::memory_order_relaxed);
}

const char* shared_queue::fence_wait_after_worst_kind(uint64_t& slot)
{
  shared_queue_state& s = state();
  slot                  = s.fence_wait_after_worst_slot.load(std::memory_order_relaxed);
  return fence_kind_name(static_cast<fence_kind>(s.fence_wait_after_worst_kind.load(std::memory_order_relaxed)));
}

namespace {
/// Which of the two process-wide queues a command buffer belongs to (Q9-F): a command buffer is bound to the
/// queue that created it, so this is the queue whose STARTS the waiter and its signaller share - and only a
/// signaller on that same queue can sit behind the waiter in a way the queue cannot resolve.
/// 0 = front end, 1 = back end, 2 = neither (a tool's own queue, or the device is gone).
unsigned queue_index_of(id<MTLCommandBuffer> command_buffer)
{
  shared_queue_state& s = state();
  if ((s.queue != nil) && (command_buffer.commandQueue == s.queue)) {
    return 0;
  }
  if ((s.backend_queue != nil) && (command_buffer.commandQueue == s.backend_queue)) {
    return 1;
  }
  return 2;
}
} // namespace

void shared_queue::note_commit_order(id<MTLCommandBuffer> command_buffer)
{
  if (command_buffer == nil) {
    return;
  }
  shared_queue_state& s      = state();
  const uint64_t      ticket = s.commit_ticket.fetch_add(1, std::memory_order_acq_rel) + 1;
  s.order_commits.fetch_add(1, std::memory_order_relaxed);
  const unsigned queue = queue_index_of(command_buffer);
  (void)ticket; // the ORDER is what the counters below encode; the number itself is not reported

  std::lock_guard<std::mutex> lock(s.order_mutex);
  shared_queue_state::order_pending_entry pending;
  auto                                    it = s.order_pending.find((__bridge const void*)command_buffer);
  if (it != s.order_pending.end()) {
    pending = std::move(it->second);
    s.order_pending.erase(it);
  }

  // ---- (1) the signals this commit carries: they advance the event, so they resolve every wait at or below
  //          the highest one - and a wait resolved here is one whose signaller came FIRST --------
  uint64_t reached = 0;
  for (uint64_t generation : pending.signals) {
    reached = std::max(reached, generation);
  }
  if (reached != 0) {
    uint64_t highest = s.order_max_signal_generation.load(std::memory_order_relaxed);
    while ((reached > highest) &&
           !s.order_max_signal_generation.compare_exchange_weak(highest, reached, std::memory_order_relaxed)) {
    }
    const uint64_t satisfied = s.order_max_signal_generation.load(std::memory_order_relaxed);
    const auto     now       = std::chrono::steady_clock::now();
    for (auto wait = s.order_open.begin(); wait != s.order_open.end();) {
      if (wait->generation > satisfied) {
        ++wait;
        continue;
      }
      // An inversion that has just been resolved by its signaller: the duration is the number to read against
      // a leg's stall, and the queue relation says whether it could have deadlocked (same queue) or only
      // delayed the waiter (the other queue runs concurrently).
      const uint64_t waited_us = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(now - wait->wait_at).count());
      if (wait->queue == queue) {
        s.order_waiter_first_same_queue.fetch_add(1, std::memory_order_relaxed);
      } else {
        s.order_waiter_first_cross_queue.fetch_add(1, std::memory_order_relaxed);
      }
      if (waited_us > s.order_worst_us.load(std::memory_order_relaxed)) {
        s.order_worst_us.store(waited_us, std::memory_order_relaxed);
        s.order_worst_slot.store(wait->slot, std::memory_order_relaxed);
        s.order_worst_kind.store(static_cast<unsigned>(wait->kind), std::memory_order_relaxed);
      }
      wait = s.order_open.erase(wait);
    }
  }

  // ---- (2) the waits this commit carries: was a signaller already committed ahead of it? ----------------
  for (size_t i = 0; i != pending.waits.size(); ++i) {
    const uint64_t     generation = pending.waits[i];
    const fence_kind   kind       = (i < pending.wait_kinds.size()) ? pending.wait_kinds[i] : fence_kind::stage;
    const uint64_t     slot       = (i < pending.wait_slots.size()) ? pending.wait_slots[i] : 0;
    const bool         has_slot   = (i < pending.wait_has_slot.size()) ? pending.wait_has_slot[i] : false;
    const auto         wait_at    = (i < pending.wait_at.size()) ? pending.wait_at[i]
                                                                : std::chrono::steady_clock::time_point{};
    s.order_waits.fetch_add(1, std::memory_order_relaxed);
    ++s.order_kind_waits[static_cast<size_t>(kind)];
    if (s.order_max_signal_generation.load(std::memory_order_relaxed) >= generation) {
      // The event can already reach this value from a command buffer committed AHEAD of this one: the wait is
      // satisfied before it starts, whatever the signaller it names does later.
      continue;
    }
    s.order_waiter_first.fetch_add(1, std::memory_order_relaxed);
    ++s.order_kind_inversions[static_cast<size_t>(kind)];
    // Remembered so the duration can be measured when (and if) a signaller commits. Bounded: a signal that
    // never comes must not grow this, and the ones dropped here are counted as unresolved at the exit.
    if (s.order_open.size() < 512) {
      shared_queue_state::order_open_wait open;
      open.generation = generation;
      open.kind       = kind;
      open.slot       = slot;
      open.queue      = queue;
      open.wait_at    = wait_at;
      s.order_open.push_back(open);
    } else {
      s.order_open_overflow.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

uint64_t shared_queue::nof_commit_order_commits()
{
  return state().order_commits.load(std::memory_order_relaxed);
}

uint64_t shared_queue::nof_commit_order_waits()
{
  return state().order_waits.load(std::memory_order_relaxed);
}

uint64_t shared_queue::nof_commit_order_waiter_first()
{
  return state().order_waiter_first.load(std::memory_order_relaxed);
}

uint64_t shared_queue::nof_commit_order_waiter_first_same_queue()
{
  return state().order_waiter_first_same_queue.load(std::memory_order_relaxed);
}

uint64_t shared_queue::nof_commit_order_waiter_first_cross_queue()
{
  return state().order_waiter_first_cross_queue.load(std::memory_order_relaxed);
}

uint64_t shared_queue::nof_commit_order_unresolved()
{
  shared_queue_state& s = state();
  // Read WITHOUT the lock on purpose: this is called from the atexit report, where taking a lock has already
  // been the cause of one crash (see state()'s note). The size is a gauge, and `order_open` is only appended
  // to and erased from under that lock by the covering threads - a report that races one of them can be off by
  // the entry being added at that instant, which changes nothing about "was anything left open".
  return static_cast<uint64_t>(s.order_open.size()) + s.order_open_overflow.load(std::memory_order_relaxed);
}

uint64_t shared_queue::nof_commit_order_kind_waits(fence_kind kind)
{
  return state().order_kind_waits[static_cast<size_t>(kind)].load(std::memory_order_relaxed);
}

uint64_t shared_queue::nof_commit_order_kind_inversions(fence_kind kind)
{
  return state().order_kind_inversions[static_cast<size_t>(kind)].load(std::memory_order_relaxed);
}

uint64_t shared_queue::commit_order_worst_us(fence_kind& kind, uint64_t& slot)
{
  shared_queue_state& s = state();
  slot                  = s.order_worst_slot.load(std::memory_order_relaxed);
  kind                  = static_cast<fence_kind>(s.order_worst_kind.load(std::memory_order_relaxed));
  return s.order_worst_us.load(std::memory_order_relaxed);
}

uint64_t shared_queue::nof_occupancy_records()
{
  shared_queue_state&         s = state();
  std::lock_guard<std::mutex> lock(s.occupancy_mutex);
  return static_cast<uint64_t>(s.occupancy.size());
}

uint64_t shared_queue::occupancy_largest_idle_us()
{
  return state().occupancy_largest_idle_us.load(std::memory_order_relaxed);
}

void shared_queue::note_stage_fence_wait(bool own_generation, bool crossed)
{
  shared_queue_state& s = state();
  if (own_generation) {
    s.stage_fence_own_waits.fetch_add(1, std::memory_order_relaxed);
  } else {
    s.stage_fence_newest_waits.fetch_add(1, std::memory_order_relaxed);
  }
  if (crossed) {
    s.stage_fence_cross_lane.fetch_add(1, std::memory_order_relaxed);
  }
}

uint64_t shared_queue::nof_stage_fence_own_waits()
{
  return state().stage_fence_own_waits.load(std::memory_order_relaxed);
}

uint64_t shared_queue::nof_stage_fence_newest_waits()
{
  return state().stage_fence_newest_waits.load(std::memory_order_relaxed);
}

uint64_t shared_queue::nof_stage_fence_cross_lane()
{
  return state().stage_fence_cross_lane.load(std::memory_order_relaxed);
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
  note_fence_wait(generation, fence_kind::stage, command_buffer);
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
