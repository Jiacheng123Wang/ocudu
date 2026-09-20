// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu_metal_mmse_engine.h"
#include "ocudu_mmse_refusals.h"

using ocudu::metal::mmse_refusals;

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "ocudu_metal_lane_clock.h"
#include "ocudu_metal_lane_probe.h"
#include "ocudu_metal_burst.h"
#include "ocudu_metal_queue.h"

#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/support/macos_compat.h"
#include "ocudu/ran/cyclic_prefix.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

#if !defined(OCUDU_MMSE_METALLIB_PATH)
#define OCUDU_MMSE_METALLIB_PATH "ocudu_mmse.metallib"
#endif

using namespace ocudu;

/// The lane stage the WEIGHTS command buffer is attributed to (see gpu_lane_probe::stage). The
/// estimator's INPUT stage keeps its own tag, so the lane report can say WHICH of the two the burst
/// spends its dependency share waiting for - "ch_est is 85% of the busy time" cannot, because both
/// command buffers of a hop used to be counted under that one name, and only one of them is what the
/// burst's equalization actually reads (gpu_h, gpu_ce, gpu_nv).
static constexpr ocudu::metal::gpu_lane_probe::stage WEIGHTS_STAGE =
    ocudu::metal::gpu_lane_probe::stage::channel_estimator_weights;

namespace {

// ---- Process-wide dispatch/wait statistics (S-1 audit probe A2) ---------------------------
// Same accounting as the LDPC engine: commits / waits / cross-thread in-flight occupancy of the
// per-engine command queues. Compile-time debug aid (ENABLE_METAL_STATS=ON defines
// OCUDU_METAL_STATS); off by default with zero overhead. Reported at process exit.
#if defined(OCUDU_METAL_STATS)
struct mmse_stats_t {
  std::atomic<uint64_t> commits{0};
  std::atomic<uint64_t> waits{0};
  std::atomic<uint64_t> in_flight{0};
  std::atomic<uint64_t> in_flight_max{0};
  // Entry-guard accounting: every submission entry waits for this engine's own outstanding batch
  // first, because the call is about to overwrite the staging buffers that batch is still reading.
  // With a single pending slot this guard is the estimator's only remaining serialization point, so
  // it is reported separately from the encode (which the phase timer shows to be ~5us).
  std::atomic<uint64_t> guard_calls{0};      // entry guards entered
  std::atomic<uint64_t> guard_hits{0};       // of those, the ones that found an outstanding batch
  std::atomic<uint64_t> guard_wait_ns{0};    // host time spent in the guards that hit
  std::atomic<uint64_t> guard_wait_max_ns{0};
  // K0-d: how many times the estimator asked the DEVICE to build the correlation matrices. This is
  // reported on purpose: an A/B against "the host builds them" proves nothing if this stayed at
  // zero for both routes, which is exactly what happened once (the device build sat in a branch the
  // captures never took, and 980 identical captures were read as agreement between two routes that
  // were in fact the same one).
  std::atomic<uint64_t> corr_builds{0};
  /// Of those builds, the ones whose correlation stage could not be encoded (the caller falls back
  /// to its host construction). Reported separately: a build that never happens and a build that
  /// fails look identical in the totals, and this is what tells them apart.
  std::atomic<uint64_t> corr_build_failures{0};
  // Glue #2 (S-7f-5u): how many pilot groups the DEVICE wrote into the engine's y slots. Counted at
  // ENCODE time, like the correlation builds, so a deferred batch is counted when it is submitted.
  // A merged hop encodes two (the standard group and the edge group), a split hop one per batch, so
  // the number is "hop groups", not hops - but it is the observable that says whether the host gave
  // up the y staging at all, which is exactly what an A/B against OCUDU_CE_DEV_Y=0 must show.
  std::atomic<uint64_t> pilots_scatters{0};
  std::atomic<uint64_t> pilots_scatter_failures{0};
  // S-7f-5w: how many hops had their noise variance computed on the device. Counted where the
  // kernels are ENCODED (the same command buffer that extracts the pilots), so it is the observable
  // that says whether the host gave up estimate_sigma2() at all.
  std::atomic<uint64_t> pilots_sigma2{0};
  // Zero-copy mappings answered by an EXISTING mapping instead of a new MTLBuffer object: every one of
  // them is a place where the cache used to create a second object over memory the first one already
  // covered, i.e. a dispatch pair whose ordering a barrier could not provide (see wrap()). The second
  // count is the subset that needed a NON-ZERO offset - the interior-pointer requests, which is exactly
  // the set that used to produce the aliased pair. A run that shows 0 there has nothing left to fix in
  // this cache; a run that shows a number is a route the barrier cannot order.
  std::atomic<uint64_t> wrap_covered{0};
  std::atomic<uint64_t> wrap_covered_offset{0};
};

static mmse_stats_t& mmse_stats()
{
  static mmse_stats_t s;
  return s;
}

static void mmse_stats_corr_build()
{
#if defined(OCUDU_METAL_STATS)
  mmse_stats().corr_builds.fetch_add(1, std::memory_order_relaxed);
#endif
}

static void mmse_stats_corr_build_failure()
{
#if defined(OCUDU_METAL_STATS)
  mmse_stats().corr_build_failures.fetch_add(1, std::memory_order_relaxed);
#endif
}

static void mmse_stats_pilots_scatter()
{
#if defined(OCUDU_METAL_STATS)
  mmse_stats().pilots_scatters.fetch_add(1, std::memory_order_relaxed);
#endif
}

static void mmse_stats_pilots_scatter_failure()
{
#if defined(OCUDU_METAL_STATS)
  mmse_stats().pilots_scatter_failures.fetch_add(1, std::memory_order_relaxed);
#endif
}

static void mmse_stats_pilots_sigma2()
{
#if defined(OCUDU_METAL_STATS)
  mmse_stats().pilots_sigma2.fetch_add(1, std::memory_order_relaxed);
#endif
}

/// Counts a wrap answered by an existing mapping (see mmse_stats_t::wrap_covered).
static void mmse_stats_wrap_covered(bool nonzero_offset)
{
#if defined(OCUDU_METAL_STATS)
  mmse_stats().wrap_covered.fetch_add(1, std::memory_order_relaxed);
  if (nonzero_offset) {
    mmse_stats().wrap_covered_offset.fetch_add(1, std::memory_order_relaxed);
  }
#else
  (void)nonzero_offset;
#endif
}

static void mmse_stats_commit()
{
  mmse_stats_t& s = mmse_stats();
  s.commits.fetch_add(1, std::memory_order_relaxed);
  const uint64_t nf = s.in_flight.fetch_add(1, std::memory_order_acq_rel) + 1;
  uint64_t       prev = s.in_flight_max.load(std::memory_order_relaxed);
  while (nf > prev && !s.in_flight_max.compare_exchange_weak(prev, nf, std::memory_order_relaxed)) {
  }
}

static void mmse_stats_wait()
{
  mmse_stats_t& s = mmse_stats();
  s.waits.fetch_add(1, std::memory_order_relaxed);
  s.in_flight.fetch_sub(1, std::memory_order_acq_rel);
}

/// \param[in] had_pending Whether the guard actually had a batch to wait for.
/// \param[in] wait_ns     Host time spent inside the guard.
static void mmse_stats_guard(bool had_pending, uint64_t wait_ns)
{
  mmse_stats_t& s = mmse_stats();
  s.guard_calls.fetch_add(1, std::memory_order_relaxed);
  if (!had_pending) {
    return;
  }
  s.guard_hits.fetch_add(1, std::memory_order_relaxed);
  s.guard_wait_ns.fetch_add(wait_ns, std::memory_order_relaxed);
  uint64_t prev = s.guard_wait_max_ns.load(std::memory_order_relaxed);
  while (wait_ns > prev && !s.guard_wait_max_ns.compare_exchange_weak(prev, wait_ns, std::memory_order_relaxed)) {
  }
}

/// Times one entry guard (see mmse_stats_t::guard_calls) into the report above.
struct mmse_guard_timer {
  bool                                  had_pending;
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();

  explicit mmse_guard_timer(bool pending) : had_pending(pending) {}
  ~mmse_guard_timer()
  {
    const auto ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    mmse_stats_guard(had_pending, static_cast<uint64_t>(ns));
  }
};

static void mmse_stats_report()
{
  const mmse_stats_t& s = mmse_stats();
  const uint64_t      hits = s.guard_hits.load(std::memory_order_relaxed);
  const uint64_t      wait = s.guard_wait_ns.load(std::memory_order_relaxed);
  std::fprintf(stderr,
               "[metal_stats] mmse_ce commits=%llu waits=%llu max_in_flight=%llu guard=%llu/%llu "
               "guard_mean=%.1fus guard_max=%.1fus device_corr_builds=%llu corr_build_fail=%llu "
               "device_y_writes=%llu y_write_fail=%llu device_sigma2=%llu refusals=",
               static_cast<unsigned long long>(s.commits.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.waits.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.in_flight_max.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(hits),
               static_cast<unsigned long long>(s.guard_calls.load(std::memory_order_relaxed)),
               (hits != 0) ? (static_cast<double>(wait) / static_cast<double>(hits) / 1e3) : 0.0,
               static_cast<double>(s.guard_wait_max_ns.load(std::memory_order_relaxed)) / 1e3,
               static_cast<unsigned long long>(s.corr_builds.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.corr_build_failures.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.pilots_scatters.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.pilots_scatter_failures.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.pilots_sigma2.load(std::memory_order_relaxed)));
  // Batch S13-P1: WHY a device stage did not run on a hop, when one did not. Printed on the same line
  // as the counts of what DID run, because the two are read together: "device_sigma2 == hops" and
  // "refusals=<none>" are the two halves of "every hop took the device route".
  mmse_refusals::print(stderr);
  // One MTLBuffer object per region of memory (see wrap()): `wrap_cover` counts the mappings an
  // existing mapping already covered - each one used to be a SECOND object over the same bytes, i.e. a
  // write/read pair no barrier in the same command buffer can order. `wrap_cover_off` is the subset with
  // a non-zero offset: the interior-pointer requests, which is the set that produced the fused route's
  // NaN. Both are printed next to the hops so an air leg can read them without a second run.
  std::fprintf(stderr,
               " wrap_cover=%llu wrap_cover_off=%llu",
               static_cast<unsigned long long>(s.wrap_covered.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.wrap_covered_offset.load(std::memory_order_relaxed)));
  std::fprintf(stderr, "\n");
}
#else  // OCUDU_METAL_STATS
static void mmse_stats_commit() {}
static void mmse_stats_wait() {}
static void mmse_stats_corr_build() {}
static void mmse_stats_corr_build_failure() {}
static void mmse_stats_pilots_scatter() {}
static void mmse_stats_pilots_scatter_failure() {}
static void mmse_stats_pilots_sigma2() {}
static void mmse_stats_wrap_covered(bool) {}

/// Stats off: the guard still has to be a non-trivially-destructible object, so that the explicit
/// scope around it does not look like an unused variable to the compiler.
struct mmse_guard_timer {
  explicit mmse_guard_timer(bool) {}
  ~mmse_guard_timer() {}
};
#endif // OCUDU_METAL_STATS

// Debug phase timer (OCUDU_MMSE_DEBUG=1): the estimator statistics show that the HOST side of an
// engine call dominates its GPU time, and this splits that host time into its parts (buffer
// wrapping, command buffer creation, encoding, commit, and the wait for the GPU).
struct mmse_phase_timer {
  const char*                   name;
  bool                          enabled;
  std::chrono::steady_clock::time_point t0, t_wrap, t_cb, t_enc, t_commit;

  explicit mmse_phase_timer(const char* n) :
    name(n), enabled(std::getenv("OCUDU_MMSE_DEBUG") != nullptr), t0(std::chrono::steady_clock::now())
  {
  }
  void wrapped() { t_wrap = std::chrono::steady_clock::now(); }
  void created() { t_cb = std::chrono::steady_clock::now(); }
  void encoded() { t_enc = std::chrono::steady_clock::now(); }
  void committed() { t_commit = std::chrono::steady_clock::now(); }
  ~mmse_phase_timer()
  {
    if (!enabled) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto us  = [](auto a, auto b) { return std::chrono::duration<double, std::micro>(b - a).count(); };
    std::fprintf(stderr,
                 "[mmse_eng] %s wrap %.1f cb %.1f encode %.1f commit %.1f wait %.1f us\n",
                 name,
                 us(t0, t_wrap),
                 us(t_wrap, t_cb),
                 us(t_cb, t_enc),
                 us(t_enc, t_commit),
                 us(t_commit, now));
  }
};

struct mmse_engine_impl {
  /// \brief Where this engine's deferred stages put their dispatches, and how the lane is ordered after
  /// them (see mmse_engine::set_lane_order() and ce_lane_order).
  ///
  /// \c host_wait, not \c event: a caller that knows nothing about the lane orders gets the behaviour this
  /// engine has always had. The receiving chain selects \c event per hop.
  metal::ce_lane_order lane_order = metal::ce_lane_order::host_wait;
  id<MTLDevice>                  device      = nil;
  id<MTLCommandQueue>            queue       = nil;
  id<MTLLibrary>                 library     = nil;
  id<MTLComputePipelineState>    inv_pipe    = nil;
  // K1b: the right-looking form of the same inversion (see ocudu_mmse_inv.metal). Used when it is
  // available; K1 stays as the fallback for a metallib that has only the block form.
  id<MTLComputePipelineState>    inv_rl_pipe = nil;
  id<MTLComputePipelineState>    weights_pipe = nil;
  id<MTLComputePipelineState>    apply_pipe  = nil;
  // K3: per-symbol, mask-compressed cbf16 estimates for the equalizer (optional, loaded on demand).
  id<MTLComputePipelineState>    reformat_pipe = nil;
  /// K5: the per-layer rsrp reduction over the same h (optional, like K3 and K4).
  id<MTLComputePipelineState>    rsrp_pipe = nil;
  /// K6 (batch 5b): the hop's time alignment, reduced from the IDFT outputs. Optional like the
  /// others: a metallib without it leaves the host's own estimator as the only source.
  id<MTLComputePipelineState>    ta_pipe = nil;
  /// K7 (batch 5b): places the hop's pilots into a DFT input, digit-reversed. Optional.
  id<MTLComputePipelineState>    ta_place_pipe = nil;
  /// Output slot of the K6 reduction. Zero-copy wrapping needs memory that is page-aligned and lives
  /// as long as the mapping (see shared_queue::wrap_no_copy), which a caller's stack float is not.
  float                          ta_out = 0.0F;
  /// ---- batch 5b: the three dispatches that produce the hop's time alignment ---------------------
  ///
  /// They run in ONE command buffer - the caller's - because the middle one's output is the last
  /// one's input, and encoding them apart would put a queue or a wait between them: the DFT engine
  /// owns the FRONT-END queue while this engine's buffers go to the BACK-END one, and command buffers
  /// of different queues have no ordering between them.
  /// K7+K6 (batch 5d): the WHOLE time-alignment chain in one dispatch (ocudu_mmse_ta.metal). It
  /// replaces the three dispatches the port started with - see encode_ta() and design doc 17.10.5.
  id<MTLComputePipelineState>    ta_chain_pipe = nil;
  id<MTLComputePipelineState>    ta_dft_pipe   = nil; // dft_dit, out of the DFT metallib (see run_ta_place)
  id<MTLBuffer>                  ta_twiddle    = nil; // N/2 roots of unity, built for ta_size
  id<MTLBuffer>                  ta_perm       = nil; // digit-reversed index, built for ta_size
  unsigned                       ta_size       = 0;   // transform size the tables were built for
  unsigned                       ta_radix2     = 0;
  unsigned                       ta_radix3     = 0;
  /// Raw pages the table wraps were built from: kept so a rebuild can release them (the wrap does
  /// not own them). Both are page-rounded allocations from build_ta_tables().
  void*                          ta_twiddle_mem = nullptr;
  void*                          ta_perm_mem    = nullptr;

  // K4: the equalizer's noise variance, reduced on the device (optional, same metallib).
  id<MTLComputePipelineState>    noise_pipe    = nil;
  // K0-d: the analytic correlation matrices A and R_hp (optional, same metallib).
  id<MTLComputePipelineState>    corr_a_pipe   = nil;
  id<MTLComputePipelineState>    corr_rhp_pipe = nil;
  // K0-a: the estimator's input stage - pilot extraction, LSE, CFO (optional, same metallib).
  id<MTLComputePipelineState>    pilots_lse_pipe   = nil;
  id<MTLComputePipelineState>    pilots_cfo_pipe   = nil;
  id<MTLComputePipelineState>    pilots_apply_pipe = nil;
  // S-7f-5w: the hop's noise variance (frequency smoothing + the classical estimator), optional on
  // its own so that a metallib without them keeps the host's estimate_sigma2().
  id<MTLComputePipelineState>    pilots_smooth_pipe = nil;
  id<MTLComputePipelineState>    pilots_sigma2_pipe = nil;

  id<MTLComputePipelineState>    pilots_power_pipe   = nil;
  // S13-P2: the received pilots' EPRE sum, reduced from the array mmse_pilots_lse() writes. Optional
  // on its own: a metallib without it leaves the host to accumulate its own statistic (which also
  // means the hop keeps the host extraction - see the base class's stage_produces_hop_inputs()).
  id<MTLComputePipelineState>    pilots_epre_pipe = nil;
  // Batch 5g: the symbol start epochs of one (numerology, CP) pair, for the unit test's exhaustive
  // comparison against the host rule. The lane never dispatches this one (see run_epoch_probe).
  id<MTLComputePipelineState>    epoch_pipe = nil;
  // Glue #2: the device writes the engine's pilot vectors out of K0-a's output (optional, same
  // metallib - a metallib without it simply keeps the host staging).
  id<MTLComputePipelineState>    pilots_scatter_pipe = nil;
  /// Submission of run_async() that has not been waited for yet (at most one, see the header).
  id<MTLCommandBuffer>           pending_cb    = nil;
  /// \brief The extraction's command buffer, held open for the weights stage (S13-P2).
  ///
  /// build_pilots_lse() normally commits and waits its own command buffer, because the host consumes
  /// its results right afterwards. When the caller promises it will not (pilots_stage::
  /// hold_for_weights), the buffer is left open here instead: the weights stage opens a SECOND encoder
  /// on it (encode_run() adopts it) and the one commit then carries both stages, which is what removes
  /// the hop's middle submission. Two encoders of one command buffer are ordered - measured, see the
  /// header - and only the ALIASED-pair trap of wrap() could break that, which is why a held buffer is
  /// only ever adopted by a stage that binds the same objects the extraction did.
  ///
  /// A held buffer is never left behind: encode_run() adopts it, and every other entry point that could
  /// be reached instead closes it (close_held_buffer(), called from wait_pending_impl() and from the
  /// entries that need their own command buffer).
  id<MTLCommandBuffer>           held_cb       = nil;
  // metal_nn_mmse: simdgroup_matrix 8x8 pipelines (optional, loaded on demand).
  id<MTLComputePipelineState>    weights_matrix_pipe = nil;
  id<MTLComputePipelineState>    apply_matrix_pipe  = nil;
  std::unordered_map<const void*, std::pair<id<MTLBuffer>, NSUInteger>> buffer_cache;
  std::mutex                                     cache_mutex;   // compute() may run on executor threads
  double                                         last_gpu_us = 0.0;

  // All Metal objects are ARC-managed (the translation unit compiles with -fobjc-arc).
  //
  // \note The TE tables and the transform scratch are deliberately NOT released here. They are mapped
  // zero-copy with a nil deallocator, and the process-wide wrap cache (shared_queue) keeps the Metal
  // buffer objects that describe those pages alive past the engine: releasing them at static
  // destruction time reaches Metal after its own state is gone, which aborts the process - measured,
  // as "mutex lock failed: Invalid argument" AFTER "All tests PASSED". The engine is a
  // process-lifetime object (one per estimator, one for the replay tool), so the pages it holds are
  // the process's, and the accounting is the OS's.
  ~mmse_engine_impl() = default;

  /// \brief TEMPORARY DIAGNOSTIC (OCUDU_CE_WRAP_MAP): what Metal actually mapped for a zero-copy request.
  ///
  /// The zero-copy wrap is the one place where the HOST's pointer and the GPU's view of it could part
  /// ways, and the fused route is the first one to wrap INTERIOR pointers on the packet path: the merged
  /// batch's edge group starts one A slot into gpu_a and one R_hp slot into gpu_r_hp (see
  /// correlation_stage()), so its base is gp + 2916 floats, not a page boundary. Metal's
  /// newBufferWithBytesNoCopy: documents that the base must be page-aligned - what it does with one that
  /// is not has to be a READING, not an assumption (the single-geometry routes never asked).
  ///
  /// One line per call: the request, the mapping handed back, and the byte offset of the request inside
  /// it. `via=cover` with a non-zero offset is a request that would have created a SECOND MTLBuffer over
  /// memory another object already covers - the shape that must never reach a dispatch pair (see wrap()).
  static bool wrap_map_enabled()
  {
    static const bool value = (std::getenv("OCUDU_CE_WRAP_MAP") != nullptr);
    return value;
  }
  void report_wrap(const void* ptr, NSUInteger bytes, id<MTLBuffer> buf, NSUInteger offset,
                   const char* how) const
  {
    if (!wrap_map_enabled()) {
      return;
    }
    const long delta = (buf != nil) ? static_cast<long>(reinterpret_cast<const char*>(buf.contents) -
                                                       reinterpret_cast<const char*>(ptr))
                                    : 0;
    std::fprintf(stderr,
                 "[wrap_map] %s ptr=%p bytes=%llu -> contents=%p length=%llu offset=%llu delta=%+ld%s\n",
                 how,
                 ptr,
                 static_cast<unsigned long long>(bytes),
                 (buf != nil) ? buf.contents : nullptr,
                 (buf != nil) ? static_cast<unsigned long long>(buf.length) : 0ULL,
                 static_cast<unsigned long long>(offset),
                 delta + static_cast<long>(offset),
                 (offset != 0) ? "  <- bound as an OFFSET into an existing mapping" : "");
  }

  /// \brief A mapping of host memory as the GPU sees it: the MTLBuffer object that owns the region and
  ///        the byte offset of the requested pointer inside it.
  ///
  /// Callers MUST bind BOTH - `setBuffer:m.buf offset:m.offset atIndex:i`. The offset is not a
  /// convenience: Metal's ordering is per MTLBuffer OBJECT, not per memory address (see wrap()).
  struct mapped {
    id<MTLBuffer> buf    = nil;
    NSUInteger    offset = 0;
  };

  /// \brief Maps host memory zero-copy, guaranteeing ONE MTLBuffer object per region of that memory.
  ///
  /// \section why_one_object Why one object per region is a correctness rule
  ///
  /// `memoryBarrierWithScope:MTLBarrierScopeBuffers` orders a write and a later read **only when both go
  /// through the same MTLBuffer object**. Two objects over the same bytes - whatever their bases and
  /// lengths - carry no ordering at all: the writer and the reader are free to run in either order, no
  /// matter how they were encoded or where the barrier sits between them. Measured in isolation, 200
  /// repetitions per case, both directions (doc_chinese/phy_pipeline_gpu/wip/metal_alias_order.mm):
  /// writer and reader on ONE object PASS, on an aliased pair FAIL every time.
  ///
  /// That is what kept the fused route (OCUDU_CE_EDGE_FUSE) computing NaN: a merged batch rides its edge
  /// group in the STANDARD group's slots, so its correlation prefix starts `sys_offset` strides into
  /// gpu_a / gpu_r_hp (correlation_stage()) and asked for an interior pointer. Keyed by pointer, the
  /// cache answered with a second MTLBuffer over memory the batch's own mapping already covered, so the
  /// inversion - reading the slots through the FIRST object, in the same command buffer, after a barrier -
  /// read them BEFORE that prefix's writes landed. The weights then read the NaN the inversion wrote, and
  /// the completion-time check saw the finished prefix, because by then the writes had landed (design doc
  /// 5.8.5, P0).
  ///
  /// So a request that an existing mapping COVERS is answered with that mapping plus an offset. The
  /// construction-time warm-up (reserve_buffer()/run_weights_only() over the staging buffers at their
  /// capacity) means the interior pointers of the matrix chain are all covered by the first mapping of
  /// each allocation, so no second object is created for them at all.
  mapped wrap(const void* ptr, NSUInteger bytes)
  {
    const char* p = static_cast<const char*>(ptr);
    {
      std::lock_guard<std::mutex> lock(cache_mutex);
      for (const auto& entry : buffer_cache) {
        const auto* base = static_cast<const char*>(entry.first);
        if ((p >= base) && (p + bytes <= base + entry.second.second)) {
          const auto offset = static_cast<NSUInteger>(p - base);
          mmse_stats_wrap_covered(offset != 0);
          report_wrap(ptr, bytes, entry.second.first, offset, "cover");
          return {entry.second.first, offset};
        }
      }
      auto it = buffer_cache.find(ptr);
      if (it != buffer_cache.end()) {
        // Same base, larger request: no cached mapping covers it, so this region has to be mapped
        // again. The entry is REPLACED (emplace() silently kept the old one, so the S-1 audit's
        // "larger request" path never took effect and every later call re-created the object).
        ocudulog::fetch_basic_logger("PHY").warning(
            "MMSE engine: zero-copy mapping has to grow past its cached extent ({} > {}): re-mapping",
            static_cast<unsigned long long>(bytes),
            static_cast<unsigned long long>(it->second.second));
      }
    }
    id<MTLBuffer> buf = [device newBufferWithBytesNoCopy:const_cast<void*>(ptr)
                                                 length:bytes
                                                options:MTLResourceStorageModeShared
                                            deallocator:nil];
    if (buf != nil) {
      std::lock_guard<std::mutex> lock(cache_mutex);
      buffer_cache.insert_or_assign(ptr, std::make_pair(buf, bytes));
    }
    report_wrap(ptr, bytes, buf, 0, (buf != nil) ? "new  " : "FAIL ");
    return {buf, 0};
  }


  /// \brief Zero-copy mapping of a buffer that another engine consumes (see
  /// reserve_shared_buffer()).
  ///
  /// Unlike wrap(), which keeps an engine-private mapping, this one goes through the process-wide
  /// cache every Metal engine shares: the stage that reads an exported tensor must bind the very
  /// same Metal buffer object the producing stage wrote through. The shared cache rounds the length
  /// up to a whole page and replaces its entry on a larger request, so exported buffers are
  /// allocated page-rounded and reserved at capacity once, at construction.
  id<MTLBuffer> wrap_shared(const void* ptr, NSUInteger bytes)
  {
    // \note The process-wide cache (shared_queue::wrap_no_copy) answers a request that an existing
    // mapping ever so slightly contains with that mapping PLUS AN OFFSET, and this entry point drops
    // that offset: it can only hand back a buffer to bind at 0. Every call site below therefore
    // depends on the request being the mapping's own base - the estimator reserves its exported
    // buffers at capacity first (see reserve_shared_buffer) so their first wrap is the whole
    // allocation, but the ROTATING slots (gpu_rsrp + slot, gpu_ta + slot) are interior pointers and
    // are the ones this would bite. The offset is logged once here rather than assumed: a non-zero
    // one is a binding to the WRONG address, and this is the line that says whether it happens.
    size_t        shared_offset = 0;
    id<MTLBuffer> buf = metal::shared_queue::wrap_no_copy(device, ptr, static_cast<size_t>(bytes), &shared_offset);
    if (buf != nil && shared_offset != 0) {
      static std::atomic<bool> offset_logged{false};
      bool                     expected = false;
      if (offset_logged.compare_exchange_strong(expected, true)) {
        ocudulog::fetch_basic_logger("PHY").error(
            "MMSE engine: the shared zero-copy cache served {} ({} bytes) as an OFFSET ({}) into an "
            "existing mapping, which this entry point cannot express: the binding below would use the "
            "mapping's base instead",
            ptr,
            static_cast<unsigned long long>(bytes),
            static_cast<unsigned long long>(shared_offset));
      }
    }
    if (buf == nil) {
      ocudulog::fetch_basic_logger("PHY").warning(
          "MMSE engine: zero-copy wrap of the exported buffer {} ({} bytes) failed",
          ptr,
          static_cast<unsigned long long>(bytes));
    }
    return buf;
  }

  bool load_library(const char* path)
  {
    NSError* err = nil;
    // The baked-in absolute source-tree path (OCUDU_MMSE_METALLIB_PATH) is the authoritative
    // load source; the NSBundle / cwd lookups below are only fallbacks (e.g. relocated builds).
    const char* primary = (path != nullptr && path[0] != '\0') ? path : OCUDU_MMSE_METALLIB_PATH;
    const char* loaded  = nullptr;
    {
      NSURL* url = [NSURL fileURLWithPath:@(primary)];
      if (url != nil) {
        library = [device newLibraryWithURL:url error:&err];
        if (library != nil) {
          loaded = primary;
        } else {
          ocudulog::fetch_basic_logger("PHY").error("MMSE engine: primary metallib load failed ({}): {}",
                                                    primary,
                                                    err != nil ? err.localizedDescription.UTF8String : "nil error");
        }
      }
    }
    if (library == nil) {
      // Fall back to a copy next to the executable or in the working directory.
      NSString* name = @"ocudu_mmse.metallib";
      NSURL*    res  = [[NSBundle mainBundle] URLForResource:name withExtension:nil];
      if (res != nil) {
        library = [device newLibraryWithURL:res error:&err];
        if (library != nil) {
          loaded = res.fileSystemRepresentation;
        }
      }
      if (library == nil) {
        library = [device newLibraryWithFile:@"ocudu_mmse.metallib" error:&err];
        if (library != nil) {
          loaded = "ocudu_mmse.metallib (cwd)";
        }
      }
    }
    if (library != nil && loaded != nullptr) {
      ocudulog::fetch_basic_logger("PHY").debug("MMSE engine: metallib loaded from {}", loaded);
    }
    return library != nil;
  }

  // Loads and compiles the metal_nn_mmse pipelines (simdgroup_matrix 8x8 kernels).
  // Called by mmse_engine::init_matrix_pipelines() after load_library(); the legacy
  // pipelines stay untouched so the engine keeps working when the kernels are absent.
  bool load_matrix_pipelines()
  {
    NSError*          err    = nil;
    id<MTLFunction>   wm_fn  = [library newFunctionWithName:@"mmse_weights_matrix"];
    id<MTLFunction>   am_fn  = [library newFunctionWithName:@"mmse_apply_matrix"];
    if (wm_fn == nil || am_fn == nil) {
      ocudulog::fetch_basic_logger("PHY").debug(
          "MMSE engine: matrix-accelerated kernels not found in the metallib (legacy kernels only)");
      return false;
    }
    weights_matrix_pipe = [device newComputePipelineStateWithFunction:wm_fn
                                                              options:MTLPipelineOptionNone
                                                           reflection:nil
                                                                error:&err];
    apply_matrix_pipe   = [device newComputePipelineStateWithFunction:am_fn
                                                              options:MTLPipelineOptionNone
                                                           reflection:nil
                                                                error:&err];
    return weights_matrix_pipe != nil && apply_matrix_pipe != nil;
  }
};

/// \brief Encoder one engine stage writes through: its own command buffer, or the shared burst.
///
/// The stages of one slot used to open, commit and WAIT a command buffer each (the synchronous contract at
/// the top of the header). In the deferred PUSCH chain that leaves two command buffers per lane on the same
/// queue with a host wait between them: Metal only orders the STARTS of a queue's command buffers, so the
/// equalizer's dispatches are not ordered after the estimates they read unless the host waits - and that
/// wait is what the fused lane removes (measured on air: 346us per call as [mmse_time_sum] gpu_wait, on top
/// of the 559us the deferred chain waits for its own command buffer).
///
/// In burst mode the stage encodes into the command buffer the chained stages share (see shared_burst): the
/// barrier that shared_burst::encoder() inserts when the pipeline changes orders it after the previous
/// stage, the lane's single commit covers it, and wait_pending() has nothing left to wait for. Failure paths
/// encode nothing further and leave the burst open; a stage that failed after encoding part of its work is
/// the one case burst mode cannot undo (the historical path simply does not commit its command buffer).
struct stage_encoder {
  id<MTLCommandBuffer>         cb    = nil;
  id<MTLComputeCommandEncoder> enc   = nil;
  bool                         burst = false;
};

static bool close_held_buffer(mmse_engine_impl* e);

/// \brief Opens a stage: the shared burst when \p fuse, the stage's own command buffer otherwise.
///
/// A HELD extraction buffer (pilots_stage::hold_for_weights) is closed first, unconditionally: it is only
/// ever ADOPTED by the weights entry point (begin_weights_stage()), and every other entry that opens a
/// command buffer here would otherwise leave the extraction's dispatches uncommitted while ITS OWN
/// dispatches - which may depend on the extraction - are already on their way to the GPU (the standalone
/// correlation build reads the extraction's noise-variance slot, so this is an ordering rule and not a
/// tidiness one).
///
/// WHICH ENTRIES MAY FUSE is a correctness question, not a performance one: the fused lane's commit
/// belongs to the receiving chain, so anything the HOST reads before that commit must not be encoded
/// into the burst. That leaves exactly the two whole-hop weight entries (run_async(),
/// run_weights_only_async()): their outputs (the estimates, the equalizer's per-symbol copies, the device
/// noise variance) are read either on the device by the later stages of the same lane or by the
/// estimator's completion, which runs after the lane committed. Every other entry - the K0-a extraction,
/// the standalone correlation build, the standalone inversion and apply - publishes something its caller
/// reads INSIDE the hop (gpu_ls_cfo / gpu_ls_sigma2 / the LSE, or A itself for the host inversion), and
/// those pass fuse = false.
static stage_encoder begin_stage(mmse_engine_impl*          e,
                                 id<MTLComputePipelineState> first_pipeline,
                                 bool                        fuse,
                                 bool                        wait_for_extraction = false)
{
  (void)close_held_buffer(e);
  stage_encoder s;
  if (fuse) {
    s.enc   = ocudu::metal::shared_burst::encoder(first_pipeline);
    s.burst = (s.enc != nil);
    if (s.burst) {
      return s;
    }
    // The burst could not be opened: fall through to the stage's own command buffer, which is what the
    // caller gets when the fusion is off - a slow lane, never a wrong one.
  }
  s.cb  = [e->queue commandBuffer];
  // Front-end fence (S-7g-17): this stage may read the resource grid the front-end DFTs produce, and a
  // wait on a command buffer of THIS queue says nothing about theirs. The wait is encoded before the
  // encoder opens (command-buffer level API) and targets the newest COMMITTED front-end generation, so
  // it can never wait for a signal that is not already on its way - see shared_queue::front_end_wait().
  ocudu::metal::shared_queue::front_end_wait(s.cb);
  // Extraction fence (S-7g-22, Step 2): this stage reads what the EXTRACTION's command buffer wrote -
  // the correlation reads the least-squares pilots and the noise variance, the reformat reads the CFO -
  // and the two are separate command buffers of one queue, whose STARTS alone are ordered (see
  // ocudu_metal_burst.mm). The extraction signals this event at its commit. Waiting on the newest
  // generation can only wait for MORE than this stage needs, never less, so a concurrent lane that
  // signals in between makes the wait conservative rather than wrong.
  //
  // \note Encoded even while the host still waits for the extraction before encoding this stage, which
  // is what makes the wait itself removable later: with the host waiting, the generation is already
  // satisfied when this is committed and the fence changes nothing observable - the two are judged
  // together by the capture nets, which stay byte-identical either way.
  //
  // \note Only in \c event order, which is the only order that uses this fence (end_stage() signals it
  // under the same condition, and the other two orders do not need it: \c burst puts the dispatches in
  // the command buffer the lane commits, \c host_wait waits on the host). It follows that no OFFLINE
  // gate covers this: a non-deferred hop is forced to host_wait by the adapter
  // (set_lane_order(args.deferred ? order : host_wait)), and the replay's hops are not deferred. The
  // air leg is the only judge - the counters to read are "[metal_stats] lane fence signals/waits".
  if (wait_for_extraction && (e->lane_order == metal::ce_lane_order::event)) {
    ocudu::metal::shared_queue::backend_stage_wait(s.cb);
  }
  s.enc = [s.cb computeCommandEncoder];
  return s;
}

/// \brief Selects the pipeline of the next dispatch, keeping the shared burst's stage tracking right.
///
/// In burst mode the switch has to GO THROUGH shared_burst::encoder(): that is what inserts the memory
/// barrier which orders this stage's dispatches after the previous stage's (the two write and read the
/// same memory through different buffer objects, so the command queue cannot order them). Calling
/// setComputePipelineState: directly would leave the burst believing no stage had changed and the
/// barrier would be missing - a silent ordering defect, not a slow path.
static id<MTLComputeCommandEncoder> stage_pipeline(mmse_engine_impl*          e,
                                                   stage_encoder&             s,
                                                   id<MTLComputePipelineState> pipe)
{
  if (!s.burst) {
    [s.enc setComputePipelineState:pipe];
    return s.enc;
  }
  id<MTLComputeCommandEncoder> enc = ocudu::metal::shared_burst::encoder(pipe);
  if (enc != nil) {
    s.enc = enc;
  }
  return s.enc;
}

/// Closes a stage: commits and waits on its own command buffer, or leaves the dispatches in the burst.
static bool end_stage(mmse_engine_impl* e, stage_encoder& s, bool encoded,
                      ocudu::metal::gpu_lane_probe::stage which =
                          ocudu::metal::gpu_lane_probe::stage::channel_estimator,
                      bool signal_extraction_fence = false)
{
  if (s.burst) {
    if (!encoded) {
      return false;
    }
    ocudu::metal::shared_burst::count_dispatch(ocudu::metal::shared_burst::stage::channel_estimator);
    // No endEncoding (the burst owns its encoder), no commit, no wait: the lane's single commit covers it.
    return true;
  }

  [s.enc endEncoding];
  if (!encoded) {
    return false;
  }
  // Extraction fence (S-7g-22): the extraction signals its own completion so the WEIGHTS command
  // buffer can encode a wait on it - which is what will let the host commit the weights WITHOUT
  // waiting first. Encoded here, immediately before the commit, for the same reason as the lane
  // burst's signal in end_stage_async(): a generation a waiting stage picks up always has a command
  // buffer on its way. Only in \c event order; the other two do not use this fence.
  if (signal_extraction_fence && (e->lane_order == metal::ce_lane_order::event)) {
    (void)ocudu::metal::shared_queue::backend_stage_signal(s.cb);
  }
  // The GPU-time probe must be armed before commit (Metal asserts otherwise).
  ocudu::metal::shared_queue::arm_gpu_time(s.cb, ocudu::metal::shared_queue::queue_kind::back_end);
  [s.cb commit];
  mmse_stats_commit();
  ocudu::metal::gpu_lane_probe::register_commit(s.cb, which);
  // Diagnostics (ocudu_metal_lane_clock.h): the lane's first command buffer exists from here on. The
  // delta from the stage entry to this commit is the part of the lane's GPU gap the HOST owns - until
  // it exists the back end has nothing queued for this lane, however idle it is.
  ocudu::metal::lane_clock.mark_extraction_commit();
  [s.cb waitUntilCompleted];
  mmse_stats_wait();

  if (s.cb.status != MTLCommandBufferStatusCompleted || s.cb.error != nil) {
    return false;
  }
  if (s.cb.GPUStartTime != 0 && s.cb.GPUEndTime != 0) {
    e->last_gpu_us = (s.cb.GPUEndTime - s.cb.GPUStartTime) * 1e6;
  }
  return true;
}

/// \brief Closes a stage whose caller collects the work later (the *_async() entries) instead of waiting in it.
///
/// The own-command-buffer form is what those entries have always done: commit, publish the command buffer as
/// the engine's pending submission, and let the caller's wait_pending() collect it. In burst mode there is
/// nothing of the engine's own to collect - the dispatches are in the command buffer the deferred chain
/// shares, and the lane's commit covers them - so the engine must publish NOTHING: a pending_cb left behind
/// would make the caller believe a submission of its own is in flight, and wait_pending() would wait for a
/// command buffer that does not exist.
///
/// In \c event order the own command buffer is additionally ARMED for the lane burst (the back-end stage
/// fence): it carries the weights, the per-symbol estimates and the noise variance the equalizer and the
/// demapper read, and those readers are in a DIFFERENT command buffer of the same queue - whose command
/// buffers only have their starts ordered. The signal is encoded here, immediately before the commit, so a
/// generation the lane burst can pick up always has a command buffer on its way (see
/// shared_queue::backend_stage_signal()).
static bool end_stage_async(mmse_engine_impl* e, stage_encoder& s, bool encoded,
                            ocudu::metal::gpu_lane_probe::stage which =
                                ocudu::metal::gpu_lane_probe::stage::channel_estimator,
                            bool mark_extraction_commit = false)
{
  if (s.burst) {
    if (!encoded) {
      return false;
    }
    ocudu::metal::shared_burst::count_dispatch(ocudu::metal::shared_burst::stage::channel_estimator);
    return true;
  }

  [s.enc endEncoding];
  if (!encoded) {
    return false;
  }
  if (e->lane_order == metal::ce_lane_order::event) {
    (void)ocudu::metal::shared_queue::backend_stage_signal(s.cb);
  }
  // The GPU-time probe must be armed before commit (Metal asserts otherwise).
  ocudu::metal::shared_queue::arm_gpu_time(s.cb, ocudu::metal::shared_queue::queue_kind::back_end);
  [s.cb commit];
  mmse_stats_commit();
  ocudu::metal::gpu_lane_probe::register_commit(s.cb, which);
  if (mark_extraction_commit) {
    // This submission carries the EXTRACTION too (it adopted the buffer build_pilots_lse() held open,
    // see pilots_stage::hold_for_weights), so it is the lane's first command buffer and the host-gap
    // reading belongs here - exactly where end_stage() puts it when the extraction commits its own.
    ocudu::metal::lane_clock.mark_extraction_commit();
  }
  e->pending_cb = s.cb;
  return true;
}

/// \brief Commits (and waits for) an extraction command buffer that was held open for the weights stage.
///
/// The counterpart of pilots_stage::hold_for_weights. While a buffer is held, the extraction's results
/// exist for the host only after this call - so it commits AND waits, because whoever reaches it (a hop
/// boundary, or an entry point that needs a command buffer of its own) is about to read them. The
/// accounting mirrors end_stage()'s for the extraction: arm the GPU-time probe before the commit, count
/// the submission, register it with the lane probe as the estimator's input stage, mark the lane's first
/// commit. \return False only when the command buffer failed.
static bool close_held_buffer(mmse_engine_impl* e)
{
  if (e->held_cb == nil) {
    return true;
  }
  id<MTLCommandBuffer> cb = e->held_cb;
  e->held_cb              = nil;
  ocudu::metal::shared_queue::arm_gpu_time(cb, ocudu::metal::shared_queue::queue_kind::back_end);
  [cb commit];
  mmse_stats_commit();
  ocudu::metal::gpu_lane_probe::register_commit(cb, ocudu::metal::gpu_lane_probe::stage::channel_estimator);
  ocudu::metal::lane_clock.mark_extraction_commit();
  [cb waitUntilCompleted];
  mmse_stats_wait();
  if (cb.status != MTLCommandBufferStatusCompleted || cb.error != nil) {
    return false;
  }
  if (cb.GPUStartTime != 0 && cb.GPUEndTime != 0) {
    e->last_gpu_us = (cb.GPUEndTime - cb.GPUStartTime) * 1e6;
  }
  return true;
}

/// \brief Gives up on a weights stage that failed part-way through its encode.
///
/// The buffer this stage holds may ALSO carry the extraction, which already succeeded - so an adopted
/// buffer is committed and waited for here instead of being dropped uncommitted: everything the caller
/// may still read (the least-squares pilots, the noise variance, the EPRE sum) must be the values THIS
/// hop's extraction produced, not the previous hop's. The partial weights dispatches run as well; the
/// caller is told the call failed and does not read their outputs (the same contract the historical
/// "encoded part of its work, never committed it" failure path had).
static void abandon_stage(mmse_engine_impl* e, stage_encoder& s, bool adopted_held)
{
  if (!s.burst) {
    [s.enc endEncoding];
  }
  if (adopted_held) {
    ocudu::metal::shared_queue::arm_gpu_time(s.cb, ocudu::metal::shared_queue::queue_kind::back_end);
    [s.cb commit];
    mmse_stats_commit();
    ocudu::metal::gpu_lane_probe::register_commit(s.cb, ocudu::metal::gpu_lane_probe::stage::channel_estimator);
    ocudu::metal::lane_clock.mark_extraction_commit();
    [s.cb waitUntilCompleted];
    mmse_stats_wait();
  }
}

/// \brief Opens the encoder the weights stage encodes into.
///
/// Two shapes, decided by whether the extraction left a command buffer held open for this stage
/// (pilots_stage::hold_for_weights):
///
///  * ADOPTED - a SECOND encoder on the EXTRACTION's command buffer, so the hop's two estimator stages
///    are one submission (S13-P2). The caller must pass \p adopted on to end_stage_async(), which marks
///    the lane's first commit on it. The fences the extraction encoded when it opened that buffer (the
///    front-end wait, and the burst's own if it had one) cover the whole buffer, and the boundary
///    between the two encoders is itself the ordering the weights need after the extraction - Metal
///    provides it for one command buffer (measured: case F of wip/metal_alias_order.mm, 200/200);
///  * otherwise - the stage's own command buffer, or the lane's shared burst in \c burst order
///    (begin_stage()). A held buffer that cannot be adopted (a nil encoder, or \c burst order, whose
///    command buffer belongs to the lane) is CLOSED AND WAITED first: the extraction-before-weights
///    order is not something this stage may skip.
static stage_encoder begin_weights_stage(mmse_engine_impl*           e,
                                         id<MTLComputePipelineState> first_pipe,
                                         bool                        fuse,
                                         bool*                       adopted)
{
  *adopted = false;
  if ((e->held_cb != nil) && !fuse) {
    id<MTLComputeCommandEncoder> held_enc = [e->held_cb computeCommandEncoder];
    if (held_enc != nil) {
      stage_encoder st;
      st.cb      = e->held_cb;
      st.enc     = held_enc;
      st.burst   = false;
      e->held_cb = nil; // this stage owns the commit now
      *adopted   = true;
      return st;
    }
  }
  (void)close_held_buffer(e);
  return begin_stage(e, first_pipe, fuse, /*wait_for_extraction=*/true);
}

/// \brief Closes an asynchronous stage and collects what its order says this caller must collect.
///
/// The three orders of ce_lane_order differ in exactly this, and nowhere else:
///  * \c burst     - the dispatches are in the lane's burst, which the lane commits and waits: this call
///                   returns \p encoded and waits for nothing (end_stage_async() published no pending
///                   command buffer, so there is nothing of the engine's to collect);
///  * \c event     - the dispatches are in the engine's own command buffer, committed as soon as they were
///                   encoded, so that the estimator's GPU work overlaps the host encoding the equalization
///                   and the demapping. The lane burst waits for it through the back-end stage fence, and
///                   the command buffer stays the engine's pending submission, collected by wait_pending()
///                   when the hop completes (which the receiving chain does after the lane, on its in-place
///                   route). Waiting here is precisely the overlap this order exists to restore - it used
///                   to cost ~125us of [ul_equalization_demod];
///  * \c host_wait - the same own command buffer, waited for HERE and now: the behaviour this engine had
///                   before the lane orders, kept as the escape hatch.
static bool collect_async_stage(mmse_engine_impl* e, stage_encoder& s, bool encoded)
{
  if (s.burst || (e->lane_order == metal::ce_lane_order::event)) {
    return encoded;
  }
  if (!encoded) {
    // Nothing was committed (see end_stage_async()): waiting for this command buffer would wait for a
    // submission that does not exist.
    return false;
  }

  [s.cb waitUntilCompleted];
  mmse_stats_wait();
  // The engine's own submission is COLLECTED now, so it must stop counting as outstanding: end_stage_async()
  // published it as pending because the caller might have collected it later, and leaving it in place made
  // the next wait_pending() wait for it a second time - which the [metal_stats] line showed as
  // waits > commits and a max_in_flight that underflowed to 2^64-1 (the host-wait order's leg).
  e->pending_cb = nil;

  if (s.cb.status != MTLCommandBufferStatusCompleted || s.cb.error != nil) {
    return false;
  }
  if (s.cb.GPUStartTime != 0 && s.cb.GPUEndTime != 0) {
    e->last_gpu_us = (s.cb.GPUEndTime - s.cb.GPUStartTime) * 1e6;
  }
  return true;
}

// Appends the K3 gather (the equalizer's per-symbol estimates) to an encoder that has just run
// K2 over h. Shared by run() and run_weights_only() so both inversion paths produce it.
/// \brief Builds the twiddle and permutation tables for one transform size (see the header).
///
/// The construction is the DFT engine's (dft_metal_engine::init): N/2 roots of unity exp(-2*pi*i*k/N)
/// and the mixed-radix digit-reversed input order, radix-2 factors first. Reproduced rather than
/// shared - the two engines own their buffers - so THIS IS THE ONE PLACE THE TWO MUST AGREE.
static bool build_ta_tables(mmse_engine_impl* e, unsigned size)
{
  if ((e->ta_size == size) && (e->ta_twiddle != nil) && (e->ta_perm != nil)) {
    return true;
  }
  if (getenv("OCUDU_CE_TA_CHECK") != nullptr) {
    fprintf(stderr, "[ta_tables] size=%u pipe=%p\n", size, (__bridge const void*)e->ta_place_pipe);
  }
  if ((size < 2u) || ((size & (size - 1u)) != 0u)) {
    // The JOB's sizes are powers of two (get_idft() rounds up); the 2^k * 3^m family is wider, but
    // the sizes this port asks for never are.
    return false;
  }
  const unsigned page  = static_cast<unsigned>(compat::page_size());
  unsigned       radix2 = 0;
  unsigned       n      = size;
  while ((n % 2u) == 0u) {
    n /= 2u;
    ++radix2;
  }
  if (n != 1u) {
    return false;
  }

  const unsigned nof_tw     = size / 2u;
  const size_t   tw_bytes   = static_cast<size_t>(nof_tw) * 2u * sizeof(float);
  const size_t   tw_rounded = (tw_bytes + page - 1u) / page * page;
  void*        tw_mem     = compat::aligned_alloc(page, tw_rounded);
  if (tw_mem == nullptr) {
    return false;
  }
  {
    auto* tw = static_cast<float*>(tw_mem);
    for (unsigned k = 0; k != nof_tw; ++k) {
      const double ang = -2.0 * M_PI * static_cast<double>(k) / static_cast<double>(size);
      tw[2 * k]        = static_cast<float>(std::cos(ang));
      tw[2 * k + 1]    = static_cast<float>(std::sin(ang));
    }
  }

  const size_t perm_bytes   = static_cast<size_t>(size) * sizeof(uint32_t);
  const size_t perm_rounded = (perm_bytes + page - 1u) / page * page;
  void*        perm_mem     = compat::aligned_alloc(page, perm_rounded);
  if (perm_mem == nullptr) {
    compat::aligned_free(tw_mem);
    return false;
  }
  {
    auto* perm = static_cast<uint32_t*>(perm_mem);
    for (uint32_t i = 0; i != size; ++i) {
      uint32_t rem       = i;
      uint32_t rev       = 0;
      uint32_t remaining = size;
      for (uint32_t q = 0; q != radix2; ++q) {
        remaining /= 2u;
        rev += (rem % 2u) * remaining;
        rem /= 2u;
      }
      perm[i] = rev;
    }
  }

  // The previous pair, if any, is dropped BEFORE the new wrap is created: the wrap cache is keyed by
  // address, and a freed block's pages are handed to the next allocation (see purge_wrap_cache).
  compat::aligned_free(e->ta_twiddle_mem);
  compat::aligned_free(e->ta_perm_mem);
  e->ta_twiddle_mem = tw_mem;
  e->ta_perm_mem    = perm_mem;
  e->ta_twiddle     = e->wrap_shared(tw_mem, tw_rounded);
  e->ta_perm        = e->wrap_shared(perm_mem, perm_rounded);
  e->ta_radix2      = radix2;
  e->ta_radix3  = 0u;
  e->ta_size    = size;
  if (getenv("OCUDU_CE_TA_CHECK") != nullptr) {
    fprintf(stderr, "[ta_tables] built: tw=%p perm=%p radix2=%u\n",
            (__bridge const void*)e->ta_twiddle, (__bridge const void*)e->ta_perm, e->ta_radix2);
  }
  return (e->ta_twiddle != nil) && (e->ta_perm != nil);
}

/// \brief Loads the DFT kernel into this engine (see mmse_engine_impl::ta_dft_pipe).
///
/// Optional like every other stage: a missing metallib leaves the host's estimator as the only
/// source of the hop's time alignment.
static bool load_ta_dft_pipeline(mmse_engine_impl* e)
{
  if (e->ta_dft_pipe != nil) {
    return true;
  }
  NSURL* url = [NSURL fileURLWithPath:@(OCUDU_DFT_METALLIB_PATH)];
  if (url == nil) {
    return false;
  }
  NSError*       err = nil;
  id<MTLLibrary> lib = [e->device newLibraryWithURL:url error:&err];
  if (lib == nil) {
    return false;
  }
  id<MTLFunction> dft_fn = [lib newFunctionWithName:@"dft_dit"];
  if (dft_fn == nil) {
    return false;
  }
  e->ta_dft_pipe = [e->device newComputePipelineStateWithFunction:dft_fn
                                                           options:MTLPipelineOptionNone
                                                        reflection:nil
                                                             error:&err];
  return e->ta_dft_pipe != nil;
}

/// \brief Encodes the hop's time alignment into the caller's command buffer (batch 5d: ONE dispatch).
///
/// The chain is placement -> transform -> power delay profile -> peak (ocudu_mmse_ta.metal,
/// mmse_ta_chain), and it runs in ONE threadgroup of one dispatch. It started as three dispatches -
/// K7, the DFT engine's dft_dit and K6 - which is the natural decomposition and still the one the
/// validation ladder gates kernel by kernel, but on air the three of them cost the lane's weights
/// command buffer ~50us/lane while the arithmetic costs nothing (design doc 17.10.5). Two experiments
/// removed the obvious candidates for that cost (the kernel's 32 KB static threadgroup scratch, and
/// the two scope-wide memory barriers between the dispatches: neither moved the measured window), which
/// leaves the dispatches themselves - so they became one.
///
/// What the fused form also removes: the intermediate spectra buffer (13 KB per hop that is no longer
/// written and read), the two cross-dispatch dependencies, and the switch between the DFT metallib and
/// this one. What it costs: the transform size is capped at the profile the kernel can keep in
/// threadgroup memory (mmse_ta_chain_max_size = 2048, which is the largest size the estimator's
/// get_idft() can ask for - 275 PRB x 6 pilots scales to exactly 2048).
///
/// The parameters are refused when they exceed the kernel's compile-time constants: those clamps are a
/// safety net, and letting them truncate a geometry silently would turn a wrong answer into one that
/// looks valid (S12_incident_gpu_hang_2026-09-19.md 6.3).
///
/// \return True when the dispatch was encoded.
static bool encode_ta(mmse_engine_impl*                             e,
                      stage_encoder&                                s,
                      const ocudu::metal::mmse_engine::reformat_stage::ta_stage_t& ta,
                      mmse_engine_impl::mapped                      h_buf,
                      const ocudu::metal::mmse_engine::hop_geometry&              geo)
{
  static constexpr unsigned kernel_max_size   = 2048; // mmse_ta_chain_max_size
  static constexpr unsigned kernel_max_slices = 16;   // mmse_ta_max_slices
  static constexpr unsigned kernel_max_dmrs   = 4;    // mmse_ta_max_dmrs
  const unsigned            nof_slices        = ta.nof_dmrs_symbols * geo.nof_layers;
  if ((h_buf.buf == nil) || (ta.dst == nullptr) || (ta.dft_size == 0u) || (ta.stride == 0u) ||
      (ta.stride > 3u) || (nof_slices == 0u) || (ta.dft_size > kernel_max_size) ||
      (nof_slices > kernel_max_slices) || (geo.nof_layers == 0u) || (geo.nof_layers > 4u) ||
      (ta.nof_dmrs_symbols == 0u) || (ta.nof_dmrs_symbols > kernel_max_dmrs) ||
      (ta.nof_dmrs_symbols > geo.nof_symbols) || (geo.nf_std == 0u) ||
      (geo.nf_std > 3300u) || (geo.nf_tail > 3300u) || (ta.max_ta_samples == 0u)) {
    return false;
  }
  for (unsigned i = 0; i != ta.nof_dmrs_symbols; ++i) {
    // Each slice reads ONE slot symbol: an index outside the slot is a parameter error the kernel
    // rejects, and refusing here is what keeps it a missing value rather than a wrong one.
    if ((ta.dmrs_slots[i] >= geo.nof_symbols) || (ta.dmrs_slots[i] >= 14u)) {
      return false;
    }
  }
  // The twiddle table and radix2 are the DFT engine's own construction, built for this size: the
  // fused kernel runs the very butterflies dft_dit does (ocudu_dft_butterflies.h), so the table it
  // reads has to be the one that kernel would have used.
  if (!build_ta_tables(e, ta.dft_size)) {
    return false;
  }
  if (e->ta_chain_pipe == nil) {
    return false;
  }

  struct mmse_ta_chain_params {
    uint32_t geo[14];
    uint32_t size;
    uint32_t stride;
    uint32_t nof_dmrs_symbols;
    uint32_t dmrs_slots[4];
    uint32_t radix2;
    uint32_t max_ta_samples;
    float    scs_hz;
    uint32_t nof_taps;
    uint32_t pad0;
    uint32_t pad1;
  };
  static_assert(sizeof(mmse_ta_chain_params) == 108,
                "mmse_ta_chain_params must match the MSL declaration");
  mmse_ta_chain_params p{};
  const auto           words = geo.words();
  for (unsigned i = 0; i != 14; ++i) {
    p.geo[i] = words[i];
  }
  p.size             = ta.dft_size;
  p.stride           = ta.stride;
  p.nof_dmrs_symbols = ta.nof_dmrs_symbols;
  for (unsigned i = 0; i != ta.max_dmrs_symbols; ++i) {
    p.dmrs_slots[i] = (i < ta.nof_dmrs_symbols) ? ta.dmrs_slots[i] : 0u;
  }
  p.radix2         = e->ta_radix2;
  p.max_ta_samples = ta.max_ta_samples;
  p.scs_hz         = ta.scs_hz;
  p.nof_taps       = (ta.max_ta_samples > 2u) ? 5u : 3u;
  p.pad0           = 0u;
  p.pad1           = 0u;

  id<MTLBuffer> out_buf = e->wrap_shared(ta.dst, sizeof(float));
  if (out_buf == nil) {
    return false;
  }
  id<MTLComputeCommandEncoder> enc = stage_pipeline(e, s, e->ta_chain_pipe);
  if (enc == nil) {
    return false;
  }
  [enc setBuffer:h_buf.buf offset:h_buf.offset atIndex:0];
  [enc setBuffer:out_buf offset:0 atIndex:1];
  [enc setBuffer:e->ta_twiddle offset:0 atIndex:2];
  [enc setBytes:&p length:sizeof(p) atIndex:3];
  [enc setBuffer:e->ta_perm offset:0 atIndex:4];
  // ONE threadgroup, min(size, 1024) threads: the same thread count the transform kernel uses, and the
  // reason the fused kernel's per-thread state arrays stay at 4 owned elements (see the shared
  // butterflies). The transform size is a compile-time bound inside the kernel; this is only how many
  // threads walk it.
  const NSUInteger threads = (ta.dft_size < 1024u) ? ta.dft_size : 1024u;
  [enc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(threads, 1, 1)];
  return true;
}


static void encode_reformat(stage_encoder&                             s,
                            mmse_engine_impl*                          e,
                            mmse_engine_impl::mapped                   h_buf,
                            const ocudu::metal::mmse_engine::reformat_stage* reformat,
                            unsigned                                   nout,
                            unsigned                                   nof_blocks)
{
  // The encoder of the stage: encode_reformat() covers two optional dispatches (K3 and K4) with a
  // pipeline of its own each, so it carries the stage rather than one encoder object.
  id<MTLComputeCommandEncoder> enc = s.enc;

  // K3 (optional): gather the equalizer's per-symbol estimates out of the h K2 has just written,
  // in the same command buffer so the hop still costs one commit and one wait.
  if (reformat != nullptr && e->reformat_pipe != nil && (reformat->dst != nullptr) &&
      (reformat->offsets != nullptr) && (reformat->nof_symbols != 0) && (reformat->nof_layers != 0) &&
      (reformat->total_re != 0)) {
    const NSUInteger dst_bytes =
        static_cast<NSUInteger>(reformat->nof_layers) * reformat->total_re * 2 * sizeof(uint16_t);
    id<MTLBuffer> dst_buf = e->wrap_shared(reformat->dst, dst_bytes);
    if (dst_buf != nil) {
      struct mmse_reformat_params {
        uint32_t nout_stride;
        uint32_t n_blk;
        uint32_t nf_std;
        uint32_t sc_tail_base;
        uint32_t nf_tail;
        uint32_t sys_tail;
        uint32_t nof_layers;
        uint32_t nof_symbols;
        uint32_t total_re;
        uint32_t dc_sc;
        uint32_t drpp;
        uint32_t drpp_dmrs;
        uint32_t dmrs_re_bits;
        uint32_t dmrs_sym_bits;
      } rparams{static_cast<uint32_t>(nout),
                static_cast<uint32_t>(nof_blocks),
                reformat->nf_std,
                reformat->nf_std * static_cast<uint32_t>(nof_blocks),
                reformat->has_tail ? reformat->nf_tail : 0u,
                reformat->sys_tail,
                reformat->nof_layers,
                reformat->nof_symbols,
                reformat->total_re,
                reformat->dc_sc,
                reformat->drpp,
                reformat->drpp_dmrs,
                reformat->dmrs_re_bits,
                reformat->dmrs_sym_bits};
      // K3 reads what K2 wrote: the one stage boundary in this command buffer where a write must
      // be made visible to a later dispatch (K1 -> K1b -> K2 have always shared an encoder and
      // rely on its in-order execution). In burst mode stage_pipeline() inserts that barrier with
      // the stage change itself.
      if (!s.burst) {
        [s.enc memoryBarrierWithScope:MTLBarrierScopeBuffers];
      }
      enc = stage_pipeline(e, s, e->reformat_pipe);
      [enc setBuffer:h_buf.buf offset:h_buf.offset atIndex:0];
      [enc setBytes:reformat->offsets
             length:static_cast<NSUInteger>(reformat->nof_symbols + 1) * sizeof(uint32_t)
             atIndex:1];
      [enc setBuffer:dst_buf offset:0 atIndex:2];
      [enc setBytes:&rparams length:sizeof(rparams) atIndex:3];
      const NSUInteger nof_sub =
          static_cast<NSUInteger>(rparams.sc_tail_base) + (reformat->has_tail ? rparams.nf_tail : 0u);
      const NSUInteger nof_threads = nof_sub * reformat->nof_symbols * reformat->nof_layers;
      [enc dispatchThreads:MTLSizeMake(nof_threads, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];

      // The hop's geometry, in the ONE struct K5 and the time-alignment placement (K7) share (see
      // hop_geometry): the two kernels read the SAME h with the same indexing, so they must not be
      // handed two copies of it. K7 is encoded below, out of this very `geo` - which is what makes
      // the claim structural rather than a comment.
      ocudu::metal::mmse_engine::hop_geometry geo{};
      geo.nout_stride  = nout;
      geo.n_blk        = reformat->rsrp.n_blk;
      geo.nf_std       = reformat->nf_std;
      geo.sc_tail_base = reformat->nf_std * nof_blocks;
      geo.nf_tail      = reformat->has_tail ? reformat->nf_tail : 0u;
      geo.sys_tail     = reformat->sys_tail;
      geo.nof_layers   = reformat->nof_layers;
      geo.nof_symbols  = reformat->nof_symbols;
      geo.dc_sc        = reformat->dc_sc;
      geo.dmrs_sym_bits = reformat->dmrs_sym_bits;
      for (unsigned l = 0; l != ocudu::metal::mmse_engine::hop_geometry::max_layers; ++l) {
        geo.pilot_re_bits[l] = (l < reformat->nof_layers) ? reformat->rsrp.pilot_re_bits[l] : 0u;
      }
      const std::array<uint32_t, 14> rpparams = geo.words();
        // The MSL side declares the same fields (ocudu_mmse_rsrp.metal); a mismatch is what turns a
        // kernel's loop bound into garbage, and a kernel that does not terminate takes the machine
        // with it (full_gpu_chain §48.131). Assert the size, and dump the fields once on request.
        static_assert(sizeof(rpparams) == 56, "mmse_rsrp_params must match the MSL declaration");

      // K5 (optional): the hop's per-layer rsrp, reduced from the SAME h this reformat just read -
      // and in the same command buffer, so the host never waits for anything to get it. It reads the
      // pilot REs K3 skips, which is why it walks h rather than dst.
      if ((reformat->rsrp.dst != nullptr) && (e->rsrp_pipe != nil) && (reformat->rsrp.n_blk != 0)) {
        static bool k5_once = false;
        if (!k5_once && (getenv("OCUDU_CE_RSRP_CHECK") != nullptr)) {
          k5_once = true;
          fprintf(stderr, "[k5] entered, dst=%p n_blk=%u\n", static_cast<void*>(reformat->rsrp.dst), reformat->rsrp.n_blk);
        }
        static bool rsrp_param_once = false;
        if (!rsrp_param_once && (getenv("OCUDU_CE_RSRP_CHECK") != nullptr)) {
          rsrp_param_once = true;
          fprintf(stderr,
                  "[rsrp_params] nout_stride=%u n_blk=%u nf_std=%u sc_tail_base=%u nf_tail=%u sys_tail=%u "
                  "layers=%u symbols=%u dc_sc=%u dmrs_sym_bits=%#x pilot0=%#x pilot1=%#x tick=%llu\n",
                  rpparams[0], rpparams[1], rpparams[2], rpparams[3],
                  rpparams[4], rpparams[5], rpparams[6], rpparams[7],
                  rpparams[8], rpparams[9], rpparams[10],
                  rpparams[11],
                  static_cast<unsigned long long>(reformat->rsrp.pilot_re_bits[0]));
        }
        // The region the kernel writes: (standard blocks + edge block slots) * layers * 2 floats.
        // ocudu_mmse_rsrp.metal lays a block slot out exactly as K2 lays out a block - layer l at
        // float (b * nof_layers + l) * 2 - so the length is that expression, and the HOST reserves
        // the same number (rsrp_region_floats()). Sizing the wrap by the standard count alone
        // TRUNCATED the mapping: the edge block's writes past the first block went nowhere, which
        // is how its (correct) reduction read back as zero.
        const unsigned rsrp_tail_slots =
            (reformat->has_tail && (reformat->nf_std != 0u))
                ? ((reformat->nf_tail + reformat->nf_std - 1u) / reformat->nf_std)
                : 0u;
        const NSUInteger rsrp_bytes = static_cast<NSUInteger>(reformat->rsrp.n_blk + rsrp_tail_slots) *
                                      reformat->nof_layers * 2u * sizeof(float);
        id<MTLBuffer> rsrp_buf = e->wrap_shared(reformat->rsrp.dst, rsrp_bytes);
        if (rsrp_buf != nil) {
          // K5 reads h, which K2 wrote and K3 also read: same producer, so the barrier K3 needed
          // already stands between them.
          enc = stage_pipeline(e, s, e->rsrp_pipe);
          [enc setBuffer:h_buf.buf offset:h_buf.offset atIndex:0];
          [enc setBuffer:rsrp_buf offset:0 atIndex:1];
          [enc setBytes:&rpparams length:sizeof(rpparams) atIndex:2];
          // One threadgroup per (block slot, layer) of the WHOLE region, edge block included: the
          // kernel derives its block slot from the subcarrier offset, so the grid must cover the
          // edge slots as well. Dispatching only the standard count left the edge block's
          // threadgroups unlaunched - the second half of "the edge block reduces nothing".
          const NSUInteger rsrp_tg = static_cast<NSUInteger>(reformat->rsrp.n_blk + rsrp_tail_slots) *
                                     reformat->nof_layers;
          [enc dispatchThreadgroups:MTLSizeMake(rsrp_tg, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        }
      }

      // K7 + the transform + K6 (optional, batch 5b): the hop's time alignment, produced entirely on
      // the device. The three dispatches go into THIS command buffer and nowhere else: the transform
      // is the DFT engine's kernel but is dispatched from here (its own engine owns the front-end
      // queue, and command buffers of different queues have no ordering between them), and K6 reads
      // what it wrote. Two of the three dispatches are the same buffer, so the barriers below are what
      // makes each one's writes visible to the next - see the header of ocudu_mmse_ta.metal.
      //
      // It is a REPORTING value like the rsrp above: it reaches the timing-advance report and the
      // debug dump, never the LLR path. That is what makes it free to move here, and it is the last
      // reason the lane still read the estimated grid back to the host on every hop.
      if ((reformat->ta.dst != nullptr) && (e->ta_place_pipe != nil) && (e->ta_pipe != nil) &&
          (e->ta_dft_pipe != nil)) {
        if (encode_ta(e, s, reformat->ta, h_buf, geo)) {
          static bool ta_once = false;
          if (!ta_once && (getenv("OCUDU_CE_TA_CHECK") != nullptr)) {
            ta_once = true;
            fprintf(stderr,
                    "[ta_params] size=%u stride=%u scs=%.0f window=%u slices=%u n_blk=%u nf_std=%u "
                    "tail=%u layers=%u sym_bits=%#x comb0=%#x\n",
                    reformat->ta.dft_size,
                    reformat->ta.stride,
                    static_cast<double>(reformat->ta.scs_hz),
                    reformat->ta.max_ta_samples,
                    reformat->ta.nof_dmrs_symbols * reformat->nof_layers,
                    geo.n_blk,
                    geo.nf_std,
                    geo.nf_tail,
                    geo.nof_layers,
                    geo.dmrs_sym_bits,
                    geo.pilot_re_bits[0]);
          }
        }
      }
    }
  }

  // K4 (optional): the noise variance the equalizer scales its soft bits with, reduced from the
  // same h - one threadgroup covers the hop. It reads the estimates at the pilot positions, hence
  // the barrier after K2 (K3 and K4 write and read disjoint buffers, so their order is free).
  const ocudu::metal::mmse_engine::reformat_stage::noise_stage_t& noise =
      (reformat != nullptr) ? reformat->noise : ocudu::metal::mmse_engine::reformat_stage::noise_stage_t{};
  static const bool k4_enabled = (std::getenv("OCUDU_CE_NO_K4") == nullptr);
  if (k4_enabled && (reformat != nullptr) && (e->noise_pipe != nil) && (noise.nv != nullptr) && (noise.pilots != nullptr) &&
      (noise.rx_pilots != nullptr) && (noise.cfo_dev != nullptr) &&
      (noise.npt != 0) && (noise.npf != 0) && (noise.comb_size != 0) && (reformat->nof_layers != 0)) {
    const NSUInteger pilots_bytes =
        static_cast<NSUInteger>(noise.npt) * reformat->nof_layers * noise.npf * 2 * sizeof(float);
    const NSUInteger rx_bytes =
        static_cast<NSUInteger>(noise.npt) * noise.nof_cdm_groups * noise.npf * 2 * sizeof(float);
    mmse_engine_impl::mapped pilots_buf = e->wrap(noise.pilots, pilots_bytes);
    mmse_engine_impl::mapped rx_buf     = e->wrap(noise.rx_pilots, rx_bytes);
    id<MTLBuffer>          nv_buf       = e->wrap_shared(noise.nv, sizeof(float));
    // The extraction's CFO, read by the device so the host never has to. It is the hop's own rotating
    // slot: see reformat_stage::noise_stage_t::cfo_dev for why one slot would not do.
    mmse_engine_impl::mapped cfo_buf = e->wrap(noise.cfo_dev, sizeof(float));
    if (pilots_buf.buf != nil && rx_buf.buf != nil && nv_buf != nil && cfo_buf.buf != nil) {
      struct mmse_noise_params {
        uint32_t nout_stride;
        uint32_t n_blk;
        uint32_t nf_std;
        uint32_t sc_tail_base;
        uint32_t nf_tail;
        uint32_t sys_tail;
        uint32_t nof_layers;
        uint32_t npt;
        uint32_t nof_cdm_groups;
        uint32_t npf;
        uint32_t nof_prb;
        uint32_t comb_size;
        uint32_t dmrs_re_bits;
        uint32_t dmrs_slots[4]; // must match mmse_noise_params and reformat_stage::noise_stage_t
        float    beta;
        float    cfo;
        uint32_t compensate_cfo;
        uint32_t cfo_from_device;
        uint32_t nof_dmrs_pilots;
        uint32_t nof_cdm;
        float    min_snr_power;
        // Batch 5g: the start epoch of each of the hop's DM-RS symbols (npt entries used), which
        // replaced the uploaded 14-float array. See mmse_noise_params::dmrs_epochs for why this
        // kernel is GIVEN them rather than deriving them.
        float    dmrs_epochs[4];
      } nparams{static_cast<uint32_t>(nout),
                static_cast<uint32_t>(nof_blocks),
                reformat->nf_std,
                reformat->nf_std * static_cast<uint32_t>(nof_blocks),
                reformat->has_tail ? reformat->nf_tail : 0u,
                reformat->sys_tail,
                reformat->nof_layers,
                noise.npt,
                noise.nof_cdm_groups,
                noise.npf,
                noise.nof_prb,
                noise.comb_size,
                noise.dmrs_re_bits,
                {},
                noise.beta,
                noise.cfo,
                noise.compensate_cfo ? 1u : 0u,
                noise.cfo_from_device ? 1u : 0u,
                noise.nof_dmrs_pilots,
                noise.nof_cdm,
                noise.min_snr_power,
                {}};
      // The parameter block is a hand-written mirror of mmse_noise_params in
      // ocudu_mmse_reformat.metal, and a wrong SIZE cannot be caught here (the kernel receives a
      // pointer) while a wrong FIELD is invisible to a size assert - both have happened in this file
      // (see the sigma2 mirror), so the offsets are pinned rather than the size.
      static_assert(offsetof(mmse_noise_params, beta) == 68, "must match mmse_noise_params::beta");
      static_assert(offsetof(mmse_noise_params, cfo) == 72, "must match mmse_noise_params::cfo");
      static_assert(offsetof(mmse_noise_params, compensate_cfo) == 76,
                    "must match mmse_noise_params::compensate_cfo");
      static_assert(offsetof(mmse_noise_params, cfo_from_device) == 80,
                    "must match mmse_noise_params::cfo_from_device");
      static_assert(offsetof(mmse_noise_params, nof_dmrs_pilots) == 84,
                    "must match mmse_noise_params::nof_dmrs_pilots");
      static_assert(offsetof(mmse_noise_params, nof_cdm) == 88, "must match mmse_noise_params::nof_cdm");
      static_assert(offsetof(mmse_noise_params, min_snr_power) == 92,
                    "must match mmse_noise_params::min_snr_power");
      static_assert(offsetof(mmse_noise_params, dmrs_epochs) == 96,
                    "must match mmse_noise_params::dmrs_epochs");
      for (unsigned i = 0; i != 4; ++i) {
        nparams.dmrs_slots[i] = noise.dmrs_slots[i];
        nparams.dmrs_epochs[i] = noise.dmrs_epochs[i];
      }
      if (!s.burst) {
        [enc memoryBarrierWithScope:MTLBarrierScopeBuffers];
      }
      enc = stage_pipeline(e, s, e->noise_pipe);
      [enc setBuffer:h_buf.buf offset:h_buf.offset atIndex:0];
      [enc setBuffer:pilots_buf.buf offset:pilots_buf.offset atIndex:1];
      [enc setBuffer:rx_buf.buf offset:rx_buf.offset atIndex:2];
      [enc setBuffer:nv_buf offset:0 atIndex:3];
      [enc setBytes:&nparams length:sizeof(nparams) atIndex:4];
      [enc setBuffer:cfo_buf.buf offset:cfo_buf.offset atIndex:5];
      // 256 = mmse_sigma2_tg_size in ocudu_mmse_pilots.metal (its reduction tree is written for it).
    [enc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
    }
  }
}

} // namespace

namespace ocudu {
namespace metal {


mmse_engine::~mmse_engine()
{
  delete static_cast<mmse_engine_impl*>(impl);
}

bool mmse_engine::init(const char* metallib_path)
{
  // Register the process-exit stats report exactly once (the counters live for the process).
#if defined(OCUDU_METAL_STATS)
  static std::once_flag stats_atexit_flag;
  std::call_once(stats_atexit_flag, []() { std::atexit(mmse_stats_report); });
#endif

  if (impl == nullptr) {
    impl = new mmse_engine_impl;
  }
  auto* e = static_cast<mmse_engine_impl*>(impl);
  if (e->device != nil) {
    return true;
  }

  e->device = MTLCreateSystemDefaultDevice();
  if (e->device == nil) {
    return false;
  }
  e->queue = metal::shared_queue::backend_queue();
  if (e->queue == nil) {
    return false;
  }
  if (!e->load_library(metallib_path)) {
    return false;
  }

  NSError* err    = nil;
  id<MTLFunction> inv_fn = [e->library newFunctionWithName:@"mmse_inv"];
  id<MTLFunction> inv_rl_fn = [e->library newFunctionWithName:@"mmse_inv_rl"];
  if (inv_rl_fn != nil) {
    e->inv_rl_pipe = [e->device newComputePipelineStateWithFunction:inv_rl_fn
                                                            options:MTLPipelineOptionNone
                                                         reflection:nil
                                                              error:&err];
  }
  id<MTLFunction> wgt_fn = [e->library newFunctionWithName:@"mmse_weights"];
  id<MTLFunction> app_fn = [e->library newFunctionWithName:@"mmse_apply"];
  if (inv_fn == nil || wgt_fn == nil || app_fn == nil) {
    return false;
  }
  e->inv_pipe = [e->device newComputePipelineStateWithFunction:inv_fn
                                                       options:MTLPipelineOptionNone
                                                    reflection:nil
                                                         error:&err];
  e->weights_pipe = [e->device newComputePipelineStateWithFunction:wgt_fn
                                                           options:MTLPipelineOptionNone
                                                        reflection:nil
                                                             error:&err];
  e->apply_pipe = [e->device newComputePipelineStateWithFunction:app_fn
                                                         options:MTLPipelineOptionNone
                                                      reflection:nil
                                                           error:&err];
  // K3 (the equalizer's per-symbol estimates) is optional: a metallib that predates it keeps the
  // estimator working, and the caller then leaves the reformat stage out of the command buffer.
  id<MTLFunction> rfmt_fn = [e->library newFunctionWithName:@"mmse_reformat"];
  if (rfmt_fn != nil) {
    e->reformat_pipe = [e->device newComputePipelineStateWithFunction:rfmt_fn
                                                              options:MTLPipelineOptionNone
                                                           reflection:nil
                                                                error:&err];
  }
  // K5 (the hop's per-layer rsrp) is optional for the same reason as K3: a metallib without it
  // leaves the host's own reduction as the only source, which is what the estimator falls back to.
  id<MTLFunction> rsrp_fn = [e->library newFunctionWithName:@"mmse_rsrp"];
  if (rsrp_fn != nil) {
    e->rsrp_pipe = [e->device newComputePipelineStateWithFunction:rsrp_fn
                                                          options:MTLPipelineOptionNone
                                                       reflection:nil
                                                            error:&err];
  }
  // K7 (batch 5b): places the hop's pilots into a DFT input. Optional, like the rest.
  (void)load_ta_dft_pipeline(e);
  if (id<MTLFunction> ta_chain_fn = [e->library newFunctionWithName:@"mmse_ta_chain"]) {
    e->ta_chain_pipe = [e->device newComputePipelineStateWithFunction:ta_chain_fn
                                                               options:MTLPipelineOptionNone
                                                            reflection:nil
                                                                 error:&err];
  }
  id<MTLFunction> ta_place_fn = [e->library newFunctionWithName:@"mmse_ta_place"];
  if (ta_place_fn != nil) {
    e->ta_place_pipe = [e->device newComputePipelineStateWithFunction:ta_place_fn
                                                              options:MTLPipelineOptionNone
                                                           reflection:nil
                                                                error:&err];
  }
  // K6 (the hop's time alignment) is optional for the same reason as K5.
  // PROVENANCE, once per process and unconditionally: WHICH time-alignment implementation this
  // metallib carries. The question "did that leg really run the fused kernel?" has already been asked
  // of a leg's numbers once, and answering it took a chain of inference (the metallib's mtime, the
  // function names inside it, and the read count proving the stage was attached). One line at startup
  // answers it instead - and a stale metallib, which this port has been bitten by before, would say so
  // here rather than in a wrong number.
  std::fprintf(stderr,
               "[ta_impl] %s\n",
               (e->ta_chain_pipe != nil) ? "fused chain: mmse_ta_chain, one dispatch (batch 5d)"
                                         : ((e->ta_place_pipe != nil) && (e->ta_pipe != nil))
                                               ? "three dispatches: mmse_ta_place + dft_dit + mmse_ta_profile"
                                               : "none: no TA kernel in the metallib, the host estimates it");
  id<MTLFunction> ta_fn = [e->library newFunctionWithName:@"mmse_ta_profile"];
  if (ta_fn != nil) {
    e->ta_pipe = [e->device newComputePipelineStateWithFunction:ta_fn
                                                        options:MTLPipelineOptionNone
                                                     reflection:nil
                                                          error:&err];
  }
  id<MTLFunction> noise_fn = [e->library newFunctionWithName:@"mmse_noise"];
  if (noise_fn != nil) {
    e->noise_pipe = [e->device newComputePipelineStateWithFunction:noise_fn
                                                           options:MTLPipelineOptionNone
                                                        reflection:nil
                                                             error:&err];
  }
  // Batch 5g's self-check (see mmse_epoch_probe): the only pipeline the lane itself never dispatches.
  // Optional like the rest, and absent from a metallib that predates it - which is what run_epoch_probe
  // reports instead of guessing.
  if (id<MTLFunction> epoch_fn = [e->library newFunctionWithName:@"mmse_epoch_probe"]) {
    e->epoch_pipe = [e->device newComputePipelineStateWithFunction:epoch_fn
                                                           options:MTLPipelineOptionNone
                                                        reflection:nil
                                                             error:&err];
  }
  // K0-a (the estimator's input stage) is optional for the same reason.
  {
    id<MTLFunction> lse_fn   = [e->library newFunctionWithName:@"mmse_pilots_lse"];
    id<MTLFunction> cfo_fn   = [e->library newFunctionWithName:@"mmse_pilots_cfo"];
    id<MTLFunction> apply_fn = [e->library newFunctionWithName:@"mmse_pilots_apply_cfo"];
    if (lse_fn != nil && cfo_fn != nil && apply_fn != nil) {
      e->pilots_lse_pipe   = [e->device newComputePipelineStateWithFunction:lse_fn options:MTLPipelineOptionNone
                                                                 reflection:nil error:&err];
      e->pilots_cfo_pipe   = [e->device newComputePipelineStateWithFunction:cfo_fn options:MTLPipelineOptionNone
                                                                 reflection:nil error:&err];
      e->pilots_apply_pipe = [e->device newComputePipelineStateWithFunction:apply_fn options:MTLPipelineOptionNone
                                                                 reflection:nil error:&err];
    }
    // Glue #2 is optional on its own: a metallib that carries K0-a but not the scatter keeps the
    // host staging, and the estimator asks through scatter_available() rather than assuming.
    id<MTLFunction> scatter_fn = [e->library newFunctionWithName:@"mmse_pilots_scatter_y"];
    if (scatter_fn != nil) {
      e->pilots_scatter_pipe = [e->device newComputePipelineStateWithFunction:scatter_fn
                                                                      options:MTLPipelineOptionNone
                                                                   reflection:nil
                                                                        error:&err];
    }
    id<MTLFunction> smooth_fn = [e->library newFunctionWithName:@"mmse_pilots_fd_smooth"];
    id<MTLFunction> sigma2_fn = [e->library newFunctionWithName:@"mmse_pilots_sigma2"];
    // The pilots' power sum rides the sigma2 block (it reduces the same pilots), so a metallib
    // without it simply does not offer the second scalar and the host keeps its own reduction.
    id<MTLFunction> power_fn = [e->library newFunctionWithName:@"mmse_pilots_power"];
    // S13-P2: the EPRE reduction over the received pilots mmse_pilots_lse() stores (see there).
    id<MTLFunction> epre_fn = [e->library newFunctionWithName:@"mmse_pilots_epre"];
    if (smooth_fn != nil && sigma2_fn != nil) {
      e->pilots_smooth_pipe = [e->device newComputePipelineStateWithFunction:smooth_fn
                                                                     options:MTLPipelineOptionNone
                                                                  reflection:nil
                                                                       error:&err];
      e->pilots_sigma2_pipe = [e->device newComputePipelineStateWithFunction:sigma2_fn
                                                                     options:MTLPipelineOptionNone
                                                                  reflection:nil
                                                                       error:&err];
      if (power_fn != nil) {
        e->pilots_power_pipe = [e->device newComputePipelineStateWithFunction:power_fn
                                                                     options:MTLPipelineOptionNone
                                                                  reflection:nil
                                                                       error:&err];
      }
    }
    if (epre_fn != nil) {
      e->pilots_epre_pipe = [e->device newComputePipelineStateWithFunction:epre_fn
                                                                    options:MTLPipelineOptionNone
                                                                 reflection:nil
                                                                      error:&err];
    }
  }

  // K0-d (the analytic correlation matrices) is optional for the same reason.
  id<MTLFunction> corr_a_fn   = [e->library newFunctionWithName:@"mmse_corr_a"];
  id<MTLFunction> corr_rhp_fn = [e->library newFunctionWithName:@"mmse_corr_r_hp"];
  if (corr_a_fn != nil && corr_rhp_fn != nil) {
    e->corr_a_pipe   = [e->device newComputePipelineStateWithFunction:corr_a_fn
                                                              options:MTLPipelineOptionNone
                                                           reflection:nil
                                                                error:&err];
    e->corr_rhp_pipe = [e->device newComputePipelineStateWithFunction:corr_rhp_fn
                                                              options:MTLPipelineOptionNone
                                                           reflection:nil
                                                                error:&err];
  }
  // ARC-managed; no explicit release.
  return e->inv_pipe != nil && e->weights_pipe != nil && e->apply_pipe != nil;
}

/// Must match mmse_pilots_params in ocudu_mmse_pilots.metal.
struct mmse_pilots_params_t {
  uint32_t nof_dmrs_symb;
  uint32_t nof_layers;
  uint32_t nof_pilots;
  uint32_t ncomb;
  uint32_t nof_prb;
  uint32_t first_prb;
  uint32_t port;
  uint32_t grid_subc_stride;
  uint32_t grid_symb_stride;
  uint32_t grid_port_stride;
  uint32_t dmrs_symb[4];
  uint32_t pilot_re[12];
  /// Batch 5g: the symbol start epochs are derived from these two instead of read from an uploaded
  /// 14-float array (see ocudu_mmse_epochs.h).
  uint32_t numerology;
  uint32_t cp_extended;
  /// Batch 5g: the start-time span of the hop's first two DM-RS symbols, for mmse_pilots_cfo (see the
  /// note in ocudu_mmse_pilots.metal: that kernel's accumulation must compile exactly as it did).
  float    epoch_span;
};
static_assert(sizeof(mmse_pilots_params_t) == 116, "mmse_pilots_params_t must match mmse_pilots_params");
// Same reason as the sigma2 struct below: a same-size swap would shift every field after it, and the
// kernels read these two by name.
static_assert(offsetof(mmse_pilots_params_t, numerology) == 104, "must match mmse_pilots_params::numerology");
static_assert(offsetof(mmse_pilots_params_t, cp_extended) == 108, "must match mmse_pilots_params::cp_extended");
static_assert(offsetof(mmse_pilots_params_t, epoch_span) == 112, "must match mmse_pilots_params::epoch_span");

/// Must match mmse_epre_params in ocudu_mmse_pilots.metal (S13-P2: the received pilots' EPRE sum).
struct mmse_epre_params_t {
  uint32_t nof_dmrs_symb;
  uint32_t nof_cdm;
  uint32_t nof_pilots;
};
static_assert(sizeof(mmse_epre_params_t) == 12, "mmse_epre_params_t must match mmse_epre_params");

/// Must match mmse_sigma2_params in ocudu_mmse_pilots.metal.
struct mmse_sigma2_params_t {
  uint32_t nof_dmrs_symb;
  uint32_t nof_layers;
  uint32_t nof_pilots;
  uint32_t nof_v_pilots;
  uint32_t filter_len;
  uint32_t nof_cdm;
  uint32_t nof_power_pilots;
  uint32_t compensate_cfo;
  float    beta;
  float    inv_beta;
  uint32_t dmrs_symb[4];
  /// Batch 5g: the symbol start epochs are derived from these two instead of read from an uploaded
  /// 14-float array (see ocudu_mmse_epochs.h). This struct is handed to THREE kernels (smooth, sigma2
  /// and power) plus its mirror in ocudu_mmse_pilots_power.metal, so the fields are appended and the
  /// offsets below are pinned.
  uint32_t numerology;
  uint32_t cp_extended;
};
static_assert(sizeof(mmse_sigma2_params_t) == 64, "mmse_sigma2_params_t must match mmse_sigma2_params");
// The fields the kernels read by name, pinned by OFFSET: a same-size swap of two fields keeps the
// size assert happy while shifting everything after them (see the corr struct's note below).
static_assert(offsetof(mmse_sigma2_params_t, nof_cdm) == 20, "must match mmse_sigma2_params::nof_cdm");
static_assert(offsetof(mmse_sigma2_params_t, nof_power_pilots) == 24,
              "must match mmse_sigma2_params::nof_power_pilots");
static_assert(offsetof(mmse_sigma2_params_t, compensate_cfo) == 28,
              "must match mmse_sigma2_params::compensate_cfo");
static_assert(offsetof(mmse_sigma2_params_t, beta) == 32, "must match mmse_sigma2_params::beta");
static_assert(offsetof(mmse_sigma2_params_t, numerology) == 56, "must match mmse_sigma2_params::numerology");
static_assert(offsetof(mmse_sigma2_params_t, cp_extended) == 60, "must match mmse_sigma2_params::cp_extended");

bool mmse_engine::build_pilots_lse(const pilots_stage& s)
{
  auto* e = static_cast<mmse_engine_impl*>(impl);
  if ((e == nullptr) || (e->device == nil) || (e->pilots_lse_pipe == nil) || (e->pilots_cfo_pipe == nil) ||
      (e->pilots_apply_pipe == nil)) {
    return false;
  }
  if ((s.grid == nullptr) || (s.grid_bytes == 0) || (s.ref == nullptr) ||
      // S13-P2: the received pilots' destination is not optional any more. The extraction kernel stores
      // what it reads into it (mmse_pilots_lse), unconditionally, so a null one would be a null store on
      // the device rather than a missing feature.
      (s.rx_pilots == nullptr) || (s.lse == nullptr) || (s.cfo == nullptr) ||
      (s.nof_dmrs_symb == 0) || (s.nof_dmrs_symb > 4) ||
      (s.nof_layers == 0) || (s.nof_layers > 4) || (s.nof_pilots == 0) || (s.nof_pilots > 3324) ||
      (s.ncomb == 0) || (s.nof_prb == 0)) {
    return false;
  }

  const NSUInteger pilots = static_cast<NSUInteger>(s.nof_dmrs_symb) * s.nof_layers * s.nof_pilots;

  mmse_engine_impl::mapped grid_buf = e->wrap(s.grid, s.grid_bytes);
  // Whole allocations, never the per-hop length: a request larger than the mapped extent forces a
  // re-map (see pilots_stage::buf_bytes).
  mmse_engine_impl::mapped ref_buf =
      e->wrap(s.ref, (s.buf_bytes != 0) ? s.buf_bytes : pilots * 2 * sizeof(float));
  mmse_engine_impl::mapped lse_buf =
      e->wrap(s.lse, (s.buf_bytes != 0) ? s.buf_bytes : pilots * 2 * sizeof(float));
  mmse_engine_impl::mapped cfo_buf = e->wrap(s.cfo, sizeof(float));
  if ((grid_buf.buf == nil) || (ref_buf.buf == nil) || (lse_buf.buf == nil) || (cfo_buf.buf == nil)) {
    return false;
  }

  // S-7f-5w: the hop's noise variance, computed here when the caller asked for it and the metallib
  // carries the kernels. Everything it needs is a buffer this call already holds - except the filter,
  // which is the host's table for the hop's geometry.
  // The kernels bound themselves with compile-time maxima (see ocudu_mmse_pilots.metal: a GPU kernel
  // must terminate for ANY parameter values, because a hang freezes the machine). A geometry that
  // exceeds those maxima would therefore be silently TRUNCATED, so it is refused here instead and the
  // host keeps its own estimate_sigma2() - the clamps are a safety net, never a truncation.
  static constexpr unsigned k_max_dmrs_symb   = 4;    // MAX_DMRS_SYMBOLS
  static constexpr unsigned k_max_layers      = 4;    // MAX_LAYERS
  static constexpr unsigned k_max_cdm         = 2;    // MAX_LAYERS / 2
  static constexpr unsigned k_max_v_pilots    = 12;   // MAX_V_PILOTS
  static constexpr unsigned k_max_filter_len  = 31;   // MAX_FILTER_LENGTH
  static constexpr unsigned k_max_pilots_symb = 3324; // MAX_NOF_SUBCARRIERS + 2 * MAX_V_PILOTS
  const bool sigma2_ok = (s.sigma2 != nullptr) && (s.smoothed != nullptr) && (s.rx_pilots != nullptr) &&
                         (s.fd_filter != nullptr) && (s.fd_filter_len != 0) && (s.nof_v_pilots != 0) &&
                         (s.nof_cdm != 0) && (e->pilots_smooth_pipe != nil) && (e->pilots_sigma2_pipe != nil) &&
                         (s.nof_dmrs_symb != 0) && (s.nof_dmrs_symb <= k_max_dmrs_symb) &&
                         (s.nof_layers <= k_max_layers) && (s.nof_cdm <= k_max_cdm) &&
                         (s.nof_v_pilots <= k_max_v_pilots) && (s.fd_filter_len <= k_max_filter_len) &&
                         (s.nof_pilots <= k_max_pilots_symb);
  // A caller that asked for the scalar must never read it back unwritten (see pilots_stage::sigma2_done):
  // report whether the stage runs, and say so loudly when it does not. The build still succeeds - the LSE
  // is unaffected - so this flag is the ONLY signal the caller gets.
  if (s.sigma2_done != nullptr) {
    *s.sigma2_done = false;
  }
  if ((s.sigma2 != nullptr) && !sigma2_ok) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      ocudulog::fetch_basic_logger("PHY").error(
          "MMSE engine: device noise variance skipped, geometry or inputs outside the kernel contract "
          "(dmrs_symb={} layers={} cdm={} v_pilots={} filter_len={} pilots={}); the host estimate is used",
          s.nof_dmrs_symb,
          s.nof_layers,
          s.nof_cdm,
          s.nof_v_pilots,
          s.fd_filter_len,
          s.nof_pilots);
    }
  }
  mmse_engine_impl::mapped smoothed_buf;
  mmse_engine_impl::mapped filt_buf;
  mmse_engine_impl::mapped rx_buf;
  mmse_engine_impl::mapped sigma2_buf;
  if (sigma2_ok) {
    smoothed_buf = e->wrap(s.smoothed, (s.buf_bytes != 0) ? s.buf_bytes : pilots * 2 * sizeof(float));
    filt_buf     = e->wrap(s.fd_filter,
                           (s.fd_filter_bytes != 0) ? s.fd_filter_bytes : s.fd_filter_len * sizeof(float));
    // FOUR floats: [0] the noise variance, [1] the pilots' power sum, [2] the noise-to-pilot-power
    // ratio the device computes and [3] the mean power it derives it from (see mmse_pilots_power).
    // The length must cover every slot the kernel writes: wrapping two made the kernel's out[2] /
    // out[3] land past the end of its Metal buffer, which is exactly the kind of out-of-bounds write
    // that surfaces as a wrong A somewhere else (measured: the CE unit test's NMSE regressed by
    // 3.9 dB at 20 dB SNR). Wrapped with its full length here and nowhere else, so no larger request
    // for it can arrive later.
    sigma2_buf = e->wrap(s.sigma2, 4 * sizeof(float));
    if ((smoothed_buf.buf == nil) || (filt_buf.buf == nil) || (sigma2_buf.buf == nil)) {
      return false;
    }
  }
  // S13-P2: the received pilots. They are an INPUT on the host-extraction routes and the extraction
  // kernel's OUTPUT on the device ones, so this buffer is wrapped whenever the caller passes one - not
  // only when the sigma2 block runs. The EPRE reduction below reads it, and so does K4 later.
  if (s.rx_pilots != nullptr) {
    rx_buf = e->wrap(s.rx_pilots, s.rx_bytes);
    if (rx_buf.buf == nil) {
      return false;
    }
  }
  mmse_engine_impl::mapped epre_buf;
  if (s.epre != nullptr) {
    if ((rx_buf.buf == nil) || (e->pilots_epre_pipe == nil)) {
      // A caller that asked for the reduction without the array to reduce, or against a metallib
      // without the kernel, would otherwise read a destination the device never wrote: refuse the
      // whole stage, which the caller handles as the host-extraction fallback.
      return false;
    }
    epre_buf = e->wrap(s.epre, sizeof(float));
    if (epre_buf.buf == nil) {
      return false;
    }
  }

  mmse_pilots_params_t p{};
  p.nof_dmrs_symb    = s.nof_dmrs_symb;
  p.nof_layers       = s.nof_layers;
  p.nof_pilots       = s.nof_pilots;
  p.ncomb            = s.ncomb;
  p.nof_prb          = s.nof_prb;
  p.first_prb        = s.first_prb;
  p.port             = s.port;
  p.grid_subc_stride = s.grid_subc_stride;
  p.grid_symb_stride = s.grid_symb_stride;
  p.grid_port_stride = s.grid_port_stride;
  for (unsigned k = 0; k != 4; ++k) {
    p.dmrs_symb[k] = s.dmrs_symb[k];
  }
  for (unsigned k = 0; k != 12; ++k) {
    p.pilot_re[k] = s.pilot_re[k];
  }
  // Batch 5g: the three kernels of this stage derive the hop's symbol start epochs from these two
  // scalars (see ocudu_mmse_epochs.h) - they used to read them out of a host-uploaded 14-float array,
  // through a device buffer this call no longer wraps or binds.
  p.numerology  = s.numerology;
  p.cp_extended = s.cp_extended ? 1u : 0u;
  p.epoch_span  = s.epoch_span;

  // NOT fused, deliberately: this stage's outputs are read by the HOST inside the same hop - the CFO
  // (gpu_ls_cfo), the noise variance and the pilots' power (gpu_ls_sigma2) feed the statistics and the
  // noise reformat that this very hop still encodes, and the LS check reads the LSE - so the command
  // buffer has to be committed and waited here (see begin_stage()).
  stage_encoder                st  = begin_stage(e, e->pilots_lse_pipe, /*fuse=*/false);
  id<MTLCommandBuffer>         cb  = st.cb;
  id<MTLComputeCommandEncoder> enc = st.enc;

  [enc setComputePipelineState:e->pilots_lse_pipe];
  [enc setBuffer:grid_buf.buf offset:grid_buf.offset atIndex:0];
  [enc setBuffer:ref_buf.buf offset:ref_buf.offset atIndex:1];
  [enc setBuffer:lse_buf.buf offset:lse_buf.offset atIndex:2];
  [enc setBytes:&p length:sizeof(p) atIndex:3];
  // The received pilots the kernel reads on its way to the product, stored into the array the noise
  // stage and K4 consume (S13-P2). A null buffer is bound when the caller passes no destination: the
  // kernel's own bounds check on it is what keeps that from being a write, and the caller then keeps
  // the host extraction.
  [enc setBuffer:rx_buf.buf offset:rx_buf.offset atIndex:4];
  [enc dispatchThreads:MTLSizeMake(s.nof_pilots, s.nof_dmrs_symb * s.nof_layers, 1)
      threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];

  [enc setComputePipelineState:e->pilots_cfo_pipe];
  [enc setBuffer:lse_buf.buf offset:lse_buf.offset atIndex:0];
  [enc setBuffer:cfo_buf.buf offset:cfo_buf.offset atIndex:1];
  [enc setBytes:&p length:sizeof(p) atIndex:2];
  // The previous hop's CFO slot, so the kernel carries a value forward itself when this hop has
  // nothing to estimate (see pilots_stage::cfo_prev). Null means the caller still carries it on the
  // host, which the kernel then reproduces by writing 0 - see the branch's note.
  mmse_engine_impl::mapped cfo_prev_buf =
      (s.cfo_prev != nullptr) ? e->wrap(s.cfo_prev, sizeof(float)) : mmse_engine_impl::mapped{};
  [enc setBuffer:cfo_prev_buf.buf offset:cfo_prev_buf.offset atIndex:3];
  [enc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];

  [enc setComputePipelineState:e->pilots_apply_pipe];
  [enc setBuffer:lse_buf.buf offset:lse_buf.offset atIndex:0];
  [enc setBuffer:cfo_buf.buf offset:cfo_buf.offset atIndex:1];
  [enc setBytes:&p length:sizeof(p) atIndex:2];
  [enc dispatchThreads:MTLSizeMake(s.nof_layers * s.nof_pilots, s.nof_dmrs_symb, 1)
      threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];

  // S-7f-5w: the hop's noise variance, from the pilots this buffer just produced. It is encoded HERE
  // (and not in a command buffer of its own) because this one is already waited for, and because the
  // host needs the scalar before it can build the correlation matrices.
  if (sigma2_ok) {
    if (s.sigma2_done != nullptr) {
      *s.sigma2_done = true;
    }
    mmse_stats_pilots_sigma2();
    mmse_sigma2_params_t q{};
    q.nof_dmrs_symb  = s.nof_dmrs_symb;
    q.nof_layers     = s.nof_layers;
    q.nof_pilots     = s.nof_pilots;
    q.nof_v_pilots   = s.nof_v_pilots;
    q.filter_len     = s.fd_filter_len;
    q.nof_cdm        = s.nof_cdm;
    // The divisor of the mean power, in the host's own order of operations (nof_layers x nof_dmrs_symb
    // x nof_pilots): the kernel writes the mean into out[3] and the ratio it derives into out[2]. Zero
    // means the caller asks for neither, and the kernel then leaves both slots at zero.
    q.nof_power_pilots = s.nof_power_pilots;
    q.compensate_cfo = (s.compensate_cfo ? 1u : 0u);
    q.beta           = s.beta;
    q.inv_beta       = s.inv_beta;
    // Batch 5g: the sigma2 kernel derives its CFO phasors' start epochs from these two (see
    // ocudu_mmse_epochs.h). The OTHER two kernels that receive this block (smooth, power) ignore
    // them; the struct has to carry them because all three read the same setBytes bytes.
    q.numerology     = s.numerology;
    q.cp_extended    = s.cp_extended ? 1u : 0u;
    for (unsigned k = 0; k != 4; ++k) {
      q.dmrs_symb[k] = s.dmrs_symb[k];
    }
    // The CFO phasors use the estimate THIS command buffer just produced (cfo_buf): the host's own
    // estimate is not known yet, and the two agree to the precision the pilots do.
    [enc setComputePipelineState:e->pilots_smooth_pipe];
    [enc setBuffer:lse_buf.buf offset:lse_buf.offset atIndex:0];
    [enc setBuffer:smoothed_buf.buf offset:smoothed_buf.offset atIndex:1];
    [enc setBuffer:filt_buf.buf offset:filt_buf.offset atIndex:2];
    [enc setBytes:&q length:sizeof(q) atIndex:3];
    // 128 = mmse_smooth_tg_size in ocudu_mmse_pilots.metal (the kernel strides its walk by it).
    [enc dispatchThreadgroups:MTLSizeMake(s.nof_dmrs_symb * s.nof_layers, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
    [enc memoryBarrierWithScope:MTLBarrierScopeBuffers];

    [enc setComputePipelineState:e->pilots_sigma2_pipe];
    [enc setBuffer:smoothed_buf.buf offset:smoothed_buf.offset atIndex:0];
    [enc setBuffer:ref_buf.buf offset:ref_buf.offset atIndex:1];
    [enc setBuffer:rx_buf.buf offset:rx_buf.offset atIndex:2];
    [enc setBuffer:cfo_buf.buf offset:cfo_buf.offset atIndex:3];
    [enc setBuffer:sigma2_buf.buf offset:sigma2_buf.offset atIndex:4];
    [enc setBytes:&q length:sizeof(q) atIndex:5];
    // 256 = mmse_sigma2_tg_size in ocudu_mmse_pilots.metal (its reduction tree is written for it).
    [enc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];

    // The pilots' mean power rides the same block: out[1] of the same buffer the sigma2 kernel wrote
    // out[0] of, so the host reads both scalars after the wait (see mmse_pilots_power).
    if (e->pilots_power_pipe != nil) {
      [enc memoryBarrierWithScope:MTLBarrierScopeBuffers];
      [enc setComputePipelineState:e->pilots_power_pipe];
      [enc setBuffer:lse_buf.buf offset:lse_buf.offset atIndex:0];
      [enc setBuffer:sigma2_buf.buf offset:sigma2_buf.offset atIndex:1];
      [enc setBytes:&q length:sizeof(q) atIndex:2];
      [enc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
    }
  }

  // S13-P2: the received pilots' EPRE sum, reduced from what the LSE kernel above stored. It reads
  // rx_buf, so it goes AFTER the barrier the sigma2 block already puts between the extraction and its
  // consumers - and it runs whenever the caller asked for it, sigma2 block or not (the EPRE statistic
  // does not depend on the noise variance).
  if (epre_buf.buf != nil) {
    if (!sigma2_ok) {
      // The barrier above is encoded inside the sigma2 block: without it, the store the LSE kernel
      // just made would not be ordered against this reduction.
      [enc memoryBarrierWithScope:MTLBarrierScopeBuffers];
    }
    mmse_epre_params_t e_params{};
    e_params.nof_dmrs_symb = s.nof_dmrs_symb;
    e_params.nof_cdm       = (s.nof_layers + 1) / 2;
    e_params.nof_pilots    = s.nof_pilots;
    [enc setComputePipelineState:e->pilots_epre_pipe];
    [enc setBuffer:rx_buf.buf offset:rx_buf.offset atIndex:0];
    [enc setBuffer:epre_buf.buf offset:epre_buf.offset atIndex:1];
    [enc setBytes:&e_params length:sizeof(e_params) atIndex:2];
    // 256 = mmse_sigma2_tg_size in ocudu_mmse_pilots.metal (the kernel's reduction tree is written
    // for it, and its walk strides by that constant).
    [enc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
  }

  // The extraction is the producer the whole rest of the hop is ordered behind, and it is the one
  // stage whose command buffer the host may later stop waiting for (Step 2): it signals the
  // extraction fence here so the weights command buffer can wait on it instead.
  //
  // S13-P2: when the caller promised that nothing on the host reads these results before the weights
  // stage is encoded (pilots_stage::hold_for_weights), the buffer is handed over INSTEAD of committed:
  // the weights open a second encoder on it and the hop then has one estimator submission rather than
  // two. Everything end_stage() does for a commit moves to whoever closes it - encode_run() (the usual
  // case) or close_held_buffer() (a hop whose weights never came) - so a held buffer is never lost and
  // never counted twice. Not honoured in \c burst order: there the weights join the lane's shared
  // command buffer, which cannot adopt this one.
  if (s.hold_for_weights && (e->lane_order != metal::ce_lane_order::burst)) {
    [st.enc endEncoding];
    e->held_cb = st.cb;
    return true;
  }
  return end_stage(e, st, true, ocudu::metal::gpu_lane_probe::stage::channel_estimator,
                   /*signal_extraction_fence=*/true);
}

/// Must match mmse_corr_params in ocudu_mmse_corr.metal.
struct mmse_corr_params_t {
  uint32_t nof_systems;
  uint32_t npt;
  uint32_t npf;
  uint32_t ncomb;
  uint32_t nf;
  uint32_t L;
  uint32_t Ls; // row stride of BOTH slots: A is [Ls][Ls], R_hp is [Ns][Ls]
  uint32_t a_sys;
  uint32_t r_sys;
  float    ts;
  float    scs_hz;
  float    fd_hz;
  float    tau_rms_s;
  float    sigma2;
  float    ridge;
  uint32_t sigma2_from_device;
  uint32_t sigma2_slot;
  uint32_t dmrs_slots[4];
  uint32_t pilot_re[12];
};

// The kernel side asserts the same size: a field added on one side only would silently shift every
// field after it (the struct crosses the boundary as opaque setBytes bytes).
static_assert(sizeof(mmse_corr_params_t) == 132, "mmse_corr_params_t must match mmse_corr_params");
// ... and a same-size swap still shifts everything after it, which the size assert cannot see. The
// fields the kernels read by name are therefore pinned by OFFSET too - the first attempt at the
// device loading added these two under one name on this side and another on the kernel's, and the
// kernel then read the wrong word as its switch (it saw "no device loading, use p.sigma2" while the
// host believed it had asked for the device's value). A mismatch must be a compile error here.
static_assert(offsetof(mmse_corr_params_t, sigma2) == 52, "must match mmse_corr_params::sigma2");
static_assert(offsetof(mmse_corr_params_t, ridge) == 56, "must match mmse_corr_params::ridge");
static_assert(offsetof(mmse_corr_params_t, sigma2_from_device) == 60,
              "must match mmse_corr_params::sigma2_from_device");
static_assert(offsetof(mmse_corr_params_t, sigma2_slot) == 64, "must match mmse_corr_params::sigma2_slot");
static_assert(offsetof(mmse_corr_params_t, dmrs_slots) == 68, "must match mmse_corr_params::dmrs_slots");

/// Encodes the two correlation dispatches of \p c into \p enc: the caller owns the command buffer,
/// so the same encoding serves the standalone entry point and the prefix of an engine call.
static bool encode_corr(mmse_engine_impl* e, stage_encoder& s, const mmse_engine::corr_stage& c,
                        unsigned nof_systems)
{
  if ((e == nullptr) || (s.enc == nil) || (e->corr_a_pipe == nil) || (e->corr_rhp_pipe == nil)) {
    return false;
  }
  if ((c.a == nullptr) || (c.r_hp == nullptr) || (nof_systems == 0) || (c.l == 0) || (c.npf == 0) ||
      (c.ncomb == 0)) {
    return false;
  }
  const unsigned npt = c.l / c.npf;
  if ((npt == 0) || (npt > 4)) {
    return false;
  }
  const unsigned nout = c.nf * MAX_NSYMB_PER_SLOT;

  // Sizes first: the kernel parameters and the zero-copy mapping both need them.
  // EXPERIMENT: the A kernel covers the slot (it writes the pad), so its dispatch and mapped extent
  // are the slot's.
  const NSUInteger a_ls        = (c.a_l_stride != 0) ? c.a_l_stride : c.l;
  const NSUInteger a_per_sys   = a_ls * a_ls;
  const NSUInteger rhp_per_sys = static_cast<NSUInteger>(nout) * c.l;
  // How much of each slot the KERNELS actually touch. Both write with the SLOT's row stride (Ls, see
  // mmse_corr_params and the two kernels: `a_sys[...o * p.Ls + col]`), so a block narrower than its
  // slot - the merged batch's edge group, L_e into the standard group's L_std slots - reaches
  // (l - 1) * Ls + l and (nout - 1) * Ls + l. The PACKED sizes above are the wrong extent for that
  // case: they are not what gets written. A zero Ls leaves the packed block (the single-geometry
  // case, where the two agree exactly).
  //
  // Measuring the difference this makes, on syn004_4 (L_e = 18 into Ls = 54, nout = 168): the mapping
  // was sized for 3024 floats while the kernel wrote up to offset 9035, so every write past the end
  // was LOST. The slot read back held data only to row 102 - the rest of the edge block's R_hp was
  // zero - while A (a square l x l block, whose extent 18 * 54 + 18 still fits inside the round-up of
  // 18 * 18) came out bit-identical. That asymmetry is exactly why k0d, which covers the standard
  // group where Ls == L, never saw this.
  const NSUInteger Ls        = a_ls;
  const NSUInteger a_extent   = a_ls * a_ls;
  const NSUInteger rhp_extent = (static_cast<NSUInteger>(nout) - 1) * Ls + c.l;
  // The batch is mapped by the SYSTEM stride, not by the packed per-system size: with a slot stride
  // wider than the block order the last system reaches past nof_systems * a_per_sys. A zero leaves
  // the packed spacing (the single-geometry case).
  const NSUInteger a_sys   = (c.a_sys_stride != 0) ? c.a_sys_stride : a_per_sys;
  const NSUInteger r_sys   = (c.r_sys_stride != 0) ? c.r_sys_stride : rhp_per_sys;
  const NSUInteger a_bytes = (static_cast<NSUInteger>(nof_systems - 1) * a_sys + a_extent) * sizeof(float);
  const NSUInteger rhp_bytes =
      (static_cast<NSUInteger>(nof_systems - 1) * r_sys + rhp_extent) * sizeof(float);

  mmse_corr_params_t p{};
  p.nof_systems = nof_systems;
  p.npt         = npt;
  p.npf         = c.npf;
  p.ncomb       = c.ncomb;
  p.nf          = c.nf;
  p.L           = c.l;
  p.Ls          = c.a_l_stride;
  p.a_sys       = static_cast<uint32_t>(a_sys);
  p.r_sys       = static_cast<uint32_t>(r_sys);
  p.ts          = c.ts;
  p.scs_hz      = c.scs_hz;
  p.fd_hz       = c.fd_hz;
  p.tau_rms_s   = c.tau_rms_s;
  p.sigma2      = c.sigma2;
  // When the caller has a device buffer, A's diagonal is loaded from the extraction's own command
  // buffer output instead of from the float above, and \c sigma2_slot says which element.
  p.sigma2_from_device = (c.sigma2_dev != nullptr) ? 1u : 0u;
  p.sigma2_slot        = c.sigma2_slot;
  // The host's diagonal ridge (build_correlation_matrices(): const float ridge = 1e-6F). Must track
  // it exactly, or the device-built A differs from the host's.
  p.ridge = 1e-6F;
  for (unsigned k = 0; k != npt; ++k) {
    p.dmrs_slots[k] = c.dmrs_slots[k];
  }
  for (unsigned k = 0; k != c.ncomb; ++k) {
    p.pilot_re[k] = c.pilot_re[k];
  }

  // ONE dispatch per matrix for the whole batch: the second grid dimension selects the system, so
  // the batch's matrices are contiguous and the kernel indexes them itself. A dispatch per system
  // measured 628us of GPU time on a 25 PRB hop - more than the host loops it replaces.
  mmse_engine_impl::mapped a_buf   = e->wrap(c.a, a_bytes);
  mmse_engine_impl::mapped rhp_buf = e->wrap(c.r_hp, rhp_bytes);
  if ((a_buf.buf == nil) || (rhp_buf.buf == nil)) {
    return false;
  }

  // Two pipelines in a row: in burst mode each switch goes through the stage's pipeline selection, so
  // the barrier that orders the A build against K1 (and the R_hp build against K2) is the burst's.
  id<MTLComputeCommandEncoder> enc = stage_pipeline(e, s, e->corr_a_pipe);
  [enc setBuffer:a_buf.buf offset:a_buf.offset atIndex:0];
  [enc setBytes:&p length:sizeof(p) atIndex:1];
  // buffer(2) is only read when p.sigma2_slot says so, but it must be bound for that kernel anyway
  // (MSL leaves an unbound device pointer undefined, and nil is not an option for a non-nullable
  // argument). \c c.sigma2_dev is the BASE of the caller's sigma2 buffer and \c p.sigma2_slot the
  // element the kernel reads, so the two agree on the address by construction - pointing the pointer
  // at the element instead would make the kernel's scalars[slot] land past the end (which is exactly
  // how this read a different element and loaded A with no noise at all).
  if (c.sigma2_dev != nullptr) {
    mmse_engine_impl::mapped sig_buf = e->wrap(c.sigma2_dev, 4 * sizeof(float));
    if (sig_buf.buf == nil) {
      return false;
    }
    [enc setBuffer:sig_buf.buf offset:sig_buf.offset atIndex:2];
  }
  [enc dispatchThreads:MTLSizeMake(a_per_sys, nof_systems, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];

  enc = stage_pipeline(e, s, e->corr_rhp_pipe);
  [enc setBuffer:rhp_buf.buf offset:rhp_buf.offset atIndex:0];
  [enc setBytes:&p length:sizeof(p) atIndex:1];
  [enc dispatchThreads:MTLSizeMake(rhp_per_sys, nof_systems, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];

  return true;
}

bool mmse_engine::build_correlation(const corr_stage& c, unsigned nof_systems)
{
  auto* e = static_cast<mmse_engine_impl*>(impl);
  if ((e == nullptr) || (e->device == nil)) {
    return false;
  }
  // NOT fused, deliberately: this entry point exists for the route where the HOST inverts what the
  // device built (the order is above K1's limit, so the host reads A out of the slots right after this
  // call). Its dispatches must therefore be complete when it returns - and a caller that only needs them
  // on the device (the prefix form inside run_async()) encodes them into its own buffer instead.
  stage_encoder st = begin_stage(e, e->corr_a_pipe, /*fuse=*/false);
  if (!encode_corr(e, st, c, nof_systems)) {
    mmse_stats_corr_build_failure();
    return false;
  }
  mmse_stats_corr_build();
  // end_stage() is the ONLY place that closes the encoder: it ends it and commits the stage's own
  // command buffer, or leaves the shared burst open for the lane to commit. Ending it here as well
  // aborted the process with "endEncoding has already been called" on every route that builds the
  // correlation matrices standalone - the OCUDU_CE_GPU_INVERT=0 escape hatch, and the whole
  // metal_nn_mmse path - and would have closed the SHARED encoder in burst mode, i.e. cut the lane's
  // single command buffer in two behind the barrier that orders its stages. It also counted the
  // build twice (the duplicate call above it).
  return end_stage(e, st, true);
}

/// Threadgroup geometry of the K1 (block Gauss-Jordan) dispatch, as (column, row) threads.
///
/// S-5a measured the blocked kernel on one 36x36 system at: (32,4) 91.3, (64,4) 67.3, (32,8) 57.0,
/// (64,8) 25.7, (32,16) 29.5, (64,16) 24.5 us - the ROW dimension (one matrix row per y thread)
/// dominates, and the spread between the best and the worst geometry (3.7x) is larger than anything
/// the arithmetic rewrite bought. (64,16) is therefore the default for every K1 dispatch.
///
/// It is a single shared helper on purpose: the inline K1 of run_async() used to hardcode (32,4) -
/// the worst of the six - so the geometry the kernel was optimized with never reached the path the
/// air interface takes, and the estimator's GPU time carried ~6x more than it had to.
/// OCUDU_INV_TGX / OCUDU_INV_TGY re-measure the sweep.
static void mmse_inv_threadgroup(unsigned& tgx, unsigned& tgy)
{
  const char* envx = std::getenv("OCUDU_INV_TGX");
  const char* envy = std::getenv("OCUDU_INV_TGY");
  tgx = (envx != nullptr) ? static_cast<unsigned>(std::strtoul(envx, nullptr, 10)) : 64;
  tgy = (envy != nullptr) ? static_cast<unsigned>(std::strtoul(envy, nullptr, 10)) : 16;
}

bool mmse_engine::invert(float* a, unsigned n, unsigned nof_systems)
{
  {
    // The local engine pointer is declared below: has_pending() is the null-safe query.
    mmse_guard_timer guard(has_pending());
    (void)wait_pending();
  }
  auto* e = static_cast<mmse_engine_impl*>(impl);
  if (e == nullptr || e->device == nil) {
    return false;
  }
  if (n > 54) {
    // The K1 kernel uses fixed-size threadgroup memory (54 x 108 floats, see ocudu_mmse_inv.metal);
    // larger systems must use the CPU inversion.
    return false;
  }

  // Phase timers (OCUDU_MMSE_DEBUG=1): the estimator statistics show that the host side of an
  // engine call dominates its GPU time, and this says which part of it.
  const bool   debug_en = (std::getenv("OCUDU_MMSE_DEBUG") != nullptr);
  const auto   t_wrap0  = std::chrono::steady_clock::now();
  const NSUInteger bytes = static_cast<NSUInteger>(nof_systems) * n * n * sizeof(float);
  mmse_engine_impl::mapped a_buf = e->wrap(a, bytes);
  if (a_buf.buf == nil) {
    return false;
  }
  const auto t_wrap1 = std::chrono::steady_clock::now();

  stage_encoder                st  = begin_stage(e, e->inv_pipe, /*fuse=*/false);
  id<MTLCommandBuffer>         cb  = st.cb;
  const auto t_cb1 = std::chrono::steady_clock::now();
  id<MTLComputeCommandEncoder> enc = st.enc;
  [enc setComputePipelineState:e->inv_pipe];
  [enc setBuffer:a_buf.buf offset:a_buf.offset atIndex:0];
  [enc setBytes:&n length:sizeof(unsigned) atIndex:1];
  [enc setBytes:&nof_systems length:sizeof(unsigned) atIndex:2];
  // One threadgroup per system, laid out as (column, row) so that the elimination of a pivot
  // column spreads over the whole block (see ocudu_mmse_inv.metal).
  {
    unsigned tgx = 0;
    unsigned tgy = 0;
    mmse_inv_threadgroup(tgx, tgy);
    [enc dispatchThreadgroups:MTLSizeMake(nof_systems, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(tgx, tgy, 1)];
  }

  return end_stage(e, st, true);
}

bool mmse_engine::apply(const float* w, const float* y, float* h, unsigned nout, unsigned L, unsigned nof_systems,
                        unsigned nof_blocks)
{
  {
    // The local engine pointer is declared below: has_pending() is the null-safe query.
    mmse_guard_timer guard(has_pending());
    (void)wait_pending();
  }
  auto* e = static_cast<mmse_engine_impl*>(impl);
  if (e == nullptr || e->device == nil) {
    return false;
  }

  mmse_engine_impl::mapped w_buf = e->wrap(w, static_cast<NSUInteger>(nof_systems) * nout * L * sizeof(float));
  mmse_engine_impl::mapped y_buf = e->wrap(y, static_cast<NSUInteger>(nof_systems) * nof_blocks * 2 * L * sizeof(float));
  mmse_engine_impl::mapped h_buf = e->wrap(h, static_cast<NSUInteger>(nof_systems) * nof_blocks * 2 * nout * sizeof(float));
  if (w_buf.buf == nil || y_buf.buf == nil || h_buf.buf == nil) {
    return false;
  }

  struct mmse_apply_params {
    uint32_t nout;
    uint32_t L;
    uint32_t nof_systems;
    uint32_t nof_blocks;
  } params{nout, L, nof_systems, nof_blocks};

  stage_encoder                st  = begin_stage(e, e->apply_pipe, /*fuse=*/false);
  id<MTLCommandBuffer>         cb  = st.cb;
  id<MTLComputeCommandEncoder> enc = st.enc;
  [enc setComputePipelineState:e->apply_pipe];
  [enc setBuffer:w_buf.buf offset:w_buf.offset atIndex:0];
  [enc setBuffer:y_buf.buf offset:y_buf.offset atIndex:1];
  [enc setBuffer:h_buf.buf offset:h_buf.offset atIndex:2];
  [enc setBytes:&params length:sizeof(params) atIndex:3];
  [enc dispatchThreadgroups:MTLSizeMake(nof_blocks * nof_systems, 1, 1)
      threadsPerThreadgroup:MTLSizeMake(nout, 1, 1)];

  return end_stage(e, st, true);
}

/// \brief Waits for the engine's own outstanding submission (the body of mmse_engine::wait_pending()).
///
/// A free function because the encode helpers below are file statics (they serve two public entry points
/// each): the wait belongs to the engine, not to the caller that happens to hold the public object.
/// \param[in] close_held See the definition: false only for the weights stage's own entry guard,
///            which is about to adopt the buffer a held extraction left open.
static bool wait_pending_impl(mmse_engine_impl* e, bool close_held = true);

/// \brief Encodes the weights-and-apply hop (K0-d prefix -> K1 -> K2 -> K3/K4) for run() and run_async().
/// \param[in] wait_for_completion True for the synchronous run(), false for run_async(). It decides
///            NOTHING about the encode except whether this hop may join the lane's burst: a synchronous
///            caller has to leave with its results ready, and only a command buffer of its own can promise
///            that (see run() for the defect that made this a parameter).
static bool encode_run(mmse_engine_impl*                  e,
                       float*                             a,
                       const float*                       r_hp,
                       float*                             w,
                       const float*                       y,
                       float*                             h,
                       unsigned                           nout,
                       unsigned                           L,
                       unsigned                           nof_systems,
                       unsigned                           nof_blocks,
                       const mmse_engine::reformat_stage* reformat,
                       const mmse_engine::corr_stage*     corr,
                       const mmse_engine::corr_stage*     corr_edge,
                       const mmse_engine::pilots_scatter* scatter,
                       unsigned                           nof_scatter,
                       bool                               wait_for_completion);

bool mmse_engine::run(float* a, const float* r_hp, float* w, const float* y, float* h, unsigned nout,
                      unsigned L, unsigned nof_systems, unsigned nof_blocks, const reformat_stage* reformat,
                      const pilots_scatter* scatter, unsigned nof_scatter)
{
  auto* e = static_cast<mmse_engine_impl*>(impl);
  if (e == nullptr || e->device == nil) {
    return false;
  }
  // WHICH ENTRY POINT the encode below serves, as a parameter and not as a property of the caller: run()
  // used to be "run_async() && wait_pending()", and the encode therefore could not tell a synchronous
  // caller from an asynchronous one. It made no difference while the fuse flag was the ADAPTER's to set
  // (it cleared it for every non-deferred hop), but the moment the flag became an engine-side order
  // (S-7g-19), a synchronous hop could join the lane's burst: its dispatches then went into a command
  // buffer no lane owns, end_stage() left them uncommitted, and the estimator returned ZEROS. The
  // synchronous contract is a property of this entry point, so it is enforced here.
  return encode_run(e,
                    a,
                    r_hp,
                    w,
                    y,
                    h,
                    nout,
                    L,
                    nof_systems,
                    nof_blocks,
                    reformat,
                    nullptr,
                    nullptr,
                    scatter,
                    nof_scatter,
                    /*wait_for_completion=*/true) &&
         wait_pending();
}

bool mmse_engine::scatter_available() const
{
  auto* e = static_cast<mmse_engine_impl*>(impl);
  return (e != nullptr) && (e->pilots_scatter_pipe != nil);
}

bool mmse_engine::epre_available() const
{
  auto* e = static_cast<mmse_engine_impl*>(impl);
  return (e != nullptr) && (e->pilots_epre_pipe != nil);
}

bool mmse_engine::rsrp_available() const
{
  auto* e = static_cast<mmse_engine_impl*>(impl);
  return (e != nullptr) && (e->rsrp_pipe != nil);
}


bool mmse_engine::ta_place_available(unsigned dft_size)
{
  auto* e = static_cast<mmse_engine_impl*>(impl);
  return (e != nullptr) && (e->ta_place_pipe != nil) && build_ta_tables(e, dft_size);
}

bool mmse_engine::run_epoch_probe(unsigned numerology, unsigned cp_extended, float* dst)
{
  auto* e = static_cast<mmse_engine_impl*>(impl);
  if ((e == nullptr) || (e->epoch_pipe == nil) || (dst == nullptr)) {
    return false;
  }

  // The destination is the caller's own 14 floats: the probe is a comparison of VALUES, not of a
  // device buffer, so the zero-copy mapping is the shortest path there and needs no copy back.
  id<MTLBuffer> dst_buf = e->wrap_shared(dst, MAX_NSYMB_PER_SLOT * sizeof(float));
  if (dst_buf == nil) {
    return false;
  }

  struct epoch_params_t {
    uint32_t numerology;
    uint32_t cp_extended;
  } p{static_cast<uint32_t>(numerology), static_cast<uint32_t>(cp_extended != 0 ? 1u : 0u)};
  static_assert(sizeof(epoch_params_t) == 8, "must match mmse_epoch_params in ocudu_mmse_reformat.metal");

  // This entry builds its own command buffer without begin_stage(), so it has to close a held
  // extraction buffer itself (see begin_stage(): a held buffer is only ever adopted by the weights
  // entry point, and anything else must commit it FIRST or the extraction's dispatches would land
  // after work that depends on them).
  (void)close_held_buffer(e);
  id<MTLCommandBuffer>         cb  = [e->queue commandBuffer];
  id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
  if (enc == nil) {
    return false;
  }
  [enc setComputePipelineState:e->epoch_pipe];
  [enc setBytes:&p length:sizeof(p) atIndex:0];
  [enc setBuffer:dst_buf offset:0 atIndex:1];
  [enc dispatchThreadgroups:MTLSizeMake(1, 1, 1)
      threadsPerThreadgroup:MTLSizeMake(static_cast<NSUInteger>(MAX_NSYMB_PER_SLOT), 1, 1)];
  [enc endEncoding];
  [cb commit];
  [cb waitUntilCompleted];
  return (cb.status == MTLCommandBufferStatusCompleted) && (cb.error == nil);
}

bool mmse_engine::run_ta_place(const void*         h,
                               const hop_geometry& geometry,
                               unsigned            dft_size,
                               unsigned            stride,
                               void*               dst)
{
  auto* e = static_cast<mmse_engine_impl*>(impl);
  if ((e == nullptr) || (e->ta_place_pipe == nil) || (h == nullptr) || (dst == nullptr) ||
      (stride == 0) || (stride > 3) || !build_ta_tables(e, dft_size)) {
    return false;
  }
  // The kernel's loops are bounded by compile-time constants, so a geometry wider than them would be
  // SILENTLY TRUNCATED - a wrong answer that looks like a valid one. Refuse it here instead: the
  // clamps inside the kernel are the safety net, never the intended limit (full_gpu_chain 48.132(e)).
  static constexpr unsigned mmse_ta_kernel_max_size   = 4096;
  static constexpr unsigned mmse_ta_kernel_max_slices = 16;
  const unsigned            nof_dmrs_symbols =
      static_cast<unsigned>(__builtin_popcount(geometry.dmrs_sym_bits));
  const unsigned nof_slices = nof_dmrs_symbols * geometry.nof_layers;
  if ((dft_size > mmse_ta_kernel_max_size) || (nof_slices == 0) || (nof_slices > mmse_ta_kernel_max_slices) ||
      (geometry.nof_layers == 0) || (geometry.nof_layers > hop_geometry::max_layers) ||
      (geometry.nf_std == 0) || (geometry.nf_std > 3300) || (geometry.nf_tail > 3300)) {
    return false;
  }
  struct mmse_ta_place_params {
    uint32_t geo[14];
    uint32_t size;
    uint32_t stride;
    uint32_t nof_dmrs_symbols;
    uint32_t dmrs_slots[4];
    uint32_t pad0;
  };
  static_assert(sizeof(mmse_ta_place_params) == 88, "mmse_ta_place_params must match the MSL declaration");
  mmse_ta_place_params p{};
  const auto           words = geometry.words();
  for (unsigned i = 0; i != 14; ++i) {
    p.geo[i] = words[i];
  }
  p.size   = dft_size;
  p.stride = stride;
  // This entry is handed a SYNTHETIC hop whose geometry describes it completely, so the hop's DM-RS
  // symbols are the set bits of dmrs_sym_bits. The lane's own path does not assume that: a hop of a
  // frequency-hopping slot carries a SUBSET of the slot's DM-RS symbols, and encode_ta() is handed
  // that subset explicitly (see reformat_stage::ta_stage_t::dmrs_slots).
  for (unsigned sym = 0; (sym != 14) && (p.nof_dmrs_symbols != 4); ++sym) {
    if (((geometry.dmrs_sym_bits >> sym) & 1u) != 0u) {
      p.dmrs_slots[p.nof_dmrs_symbols++] = sym;
    }
  }
  if (p.nof_dmrs_symbols == 0) {
    return false;
  }

  // Everything here is a RUN of slices, so each wrap is one range: no per-slice blit, no offsets. The
  // h buffer is the caller's (the estimator's staging), the destination is its own run of slices.
  //
  // The length is the WHOLE batch h can span: nof_systems rows of n_blk block slots, where the edge
  // block - when the geometry has one - lives in the systems from sys_tail on. Sizing it by the
  // layers alone would leave the edge rows outside the mapping (that is "the edge block reduces
  // nothing" again, one kernel over), and over-asking is the safe direction: a wrap that reaches past
  // its allocation is REFUSED by shared_queue::wrap_no_copy, never silently shortened.
  const unsigned   n_sys = std::max(geometry.nof_layers,
                                    (geometry.nf_tail != 0u) ? (geometry.sys_tail + geometry.nof_layers) : 0u);
  const NSUInteger h_bytes =
      static_cast<NSUInteger>(n_sys) * geometry.n_blk * 2u * geometry.nout_stride * sizeof(float);
  id<MTLBuffer> h_buf   = e->wrap_shared(h, h_bytes);
  id<MTLBuffer> dst_buf = e->wrap_shared(dst, static_cast<NSUInteger>(nof_slices) * dft_size * 2u * sizeof(float));
  if ((h_buf == nil) || (dst_buf == nil)) {
    return false;
  }

  // This entry builds its own command buffer without begin_stage(), so it has to close a held
  // extraction buffer itself (see begin_stage(): a held buffer is only ever adopted by the weights
  // entry point, and anything else must commit it FIRST or the extraction's dispatches would land
  // after work that depends on them).
  (void)close_held_buffer(e);
  id<MTLCommandBuffer>         cb  = [e->queue commandBuffer];
  id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
  if (enc == nil) {
    return false;
  }
  [enc setComputePipelineState:e->ta_place_pipe];
  [enc setBuffer:h_buf offset:0 atIndex:0];
  // index 1 is unused: the placement is in natural order, so no permutation table is read here.
  [enc setBuffer:dst_buf offset:0 atIndex:2];
  [enc setBytes:&p length:sizeof(p) atIndex:3];
  [enc dispatchThreadgroups:MTLSizeMake(nof_slices, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
  [enc endEncoding];
  [cb commit];
  [cb waitUntilCompleted];
  return (cb.status == MTLCommandBufferStatusCompleted) && (cb.error == nil);
}

bool mmse_engine::ta_chain_available(unsigned dft_size) const
{
  auto* e = static_cast<const mmse_engine_impl*>(impl);
  return (e != nullptr) && (e->ta_chain_pipe != nil) && (dft_size != 0) && (dft_size <= 2048);
}

bool mmse_engine::run_ta_chain(const void*         h,
                               const hop_geometry& geometry,
                               unsigned            dft_size,
                               unsigned            stride,
                               unsigned            nof_dmrs_symbols,
                               const unsigned*     dmrs_slots,
                               double              scs_hz,
                               unsigned            max_ta_samples,
                               void*               ta_seconds)
{
  auto* e = static_cast<mmse_engine_impl*>(impl);
  if ((e == nullptr) || (e->ta_chain_pipe == nil) || (h == nullptr) || (ta_seconds == nullptr) ||
      (dmrs_slots == nullptr) || (dft_size == 0) || (dft_size > 2048) || (stride == 0) || (stride > 3) ||
      (nof_dmrs_symbols == 0) || (nof_dmrs_symbols > hop_geometry::max_layers) || (scs_hz <= 0.0) ||
      (max_ta_samples == 0) || !build_ta_tables(e, dft_size)) {
    return false;
  }
  // The kernel's loops are bounded by compile-time constants, so a geometry wider than them would be
  // SILENTLY TRUNCATED - a wrong answer that looks like a valid one. Refuse it here instead: the
  // clamps inside the kernel are the safety net, never the intended limit (full_gpu_chain 48.132(e)).
  const unsigned nof_slices = nof_dmrs_symbols * geometry.nof_layers;
  if ((nof_slices == 0) || (nof_slices > 16) || (geometry.nof_layers == 0) ||
      (geometry.nof_layers > hop_geometry::max_layers) || (geometry.nf_std == 0) ||
      (geometry.nf_std > 3300) || (geometry.nf_tail > 3300)) {
    return false;
  }

  struct mmse_ta_chain_params {
    uint32_t geo[14];
    uint32_t size;
    uint32_t stride;
    uint32_t nof_dmrs_symbols;
    uint32_t dmrs_slots[4];
    uint32_t radix2;
    uint32_t max_ta_samples;
    float    scs_hz;
    uint32_t nof_taps;
    uint32_t pad0;
    uint32_t pad1;
  };
  static_assert(sizeof(mmse_ta_chain_params) == 108, "mmse_ta_chain_params must match the MSL declaration");
  mmse_ta_chain_params p{};
  const auto           words = geometry.words();
  for (unsigned i = 0; i != 14; ++i) {
    p.geo[i] = words[i];
  }
  p.size             = dft_size;
  p.stride           = stride;
  p.nof_dmrs_symbols = nof_dmrs_symbols;
  for (unsigned i = 0; i != 4; ++i) {
    p.dmrs_slots[i] = (i < nof_dmrs_symbols) ? dmrs_slots[i] : 0u;
  }
  p.radix2         = e->ta_radix2;
  p.max_ta_samples = max_ta_samples;
  p.scs_hz         = static_cast<float>(scs_hz);
  p.nof_taps       = (max_ta_samples > 2u) ? 5u : 3u;

  // One run of the batch geometry, so each wrap is one range (see run_ta_place for the length's
  // reasoning: the whole batch h can span, edge systems included).
  const unsigned   n_sys = std::max(geometry.nof_layers,
                                    (geometry.nf_tail != 0u) ? (geometry.sys_tail + geometry.nof_layers) : 0u);
  const NSUInteger h_bytes =
      static_cast<NSUInteger>(n_sys) * geometry.n_blk * 2u * geometry.nout_stride * sizeof(float);
  id<MTLBuffer> h_buf   = e->wrap_shared(h, h_bytes);
  id<MTLBuffer> out_buf = e->wrap_shared(ta_seconds, sizeof(float));
  if ((h_buf == nil) || (out_buf == nil)) {
    return false;
  }

  // This entry builds its own command buffer without begin_stage(), so it has to close a held
  // extraction buffer itself (see begin_stage(): a held buffer is only ever adopted by the weights
  // entry point, and anything else must commit it FIRST or the extraction's dispatches would land
  // after work that depends on them).
  (void)close_held_buffer(e);
  id<MTLCommandBuffer>         cb  = [e->queue commandBuffer];
  id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
  if (enc == nil) {
    return false;
  }
  [enc setComputePipelineState:e->ta_chain_pipe];
  [enc setBuffer:h_buf offset:0 atIndex:0];
  [enc setBuffer:out_buf offset:0 atIndex:1];
  [enc setBuffer:e->ta_twiddle offset:0 atIndex:2];
  [enc setBytes:&p length:sizeof(p) atIndex:3];
  [enc setBuffer:e->ta_perm offset:0 atIndex:4];
  const NSUInteger threads = (dft_size < 1024u) ? dft_size : 1024u;
  [enc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(threads, 1, 1)];
  [enc endEncoding];
  [cb commit];
  [cb waitUntilCompleted];
  return (cb.status == MTLCommandBufferStatusCompleted) && (cb.error == nil);
}

bool mmse_engine::ta_available() const
{
  auto* e = static_cast<const mmse_engine_impl*>(impl);
  return (e != nullptr) && (e->ta_pipe != nil);
}

bool mmse_engine::run_ta_profile(const void* slices,
                                 unsigned    size,
                                 unsigned    nof_slices,
                                 unsigned    stride,
                                 double      scs_hz,
                                 unsigned    max_ta_samples,
                                 float&      ta_seconds)
{
  auto* e = static_cast<mmse_engine_impl*>(impl);
  if ((e == nullptr) || (e->ta_pipe == nil) || (slices == nullptr) || (size == 0) || (nof_slices == 0) ||
      (stride == 0) || (scs_hz <= 0.0)) {
    return false;
  }
  // See run_ta_place(): refuse a geometry wider than the kernel's constants rather than let the
  // clamps truncate it silently.
  static constexpr unsigned mmse_ta_kernel_max_size   = 4096;
  static constexpr unsigned mmse_ta_kernel_max_slices = 16;
  if ((size > mmse_ta_kernel_max_size) || (nof_slices > mmse_ta_kernel_max_slices)) {
    return false;
  }
  // The kernel's parameter block, field for field (see ocudu_mmse_ta.metal).
  struct mmse_ta_params {
    uint32_t size;
    uint32_t nof_slices;
    uint32_t stride;
    float    scs_hz;
    uint32_t max_ta_samples;
    uint32_t nof_taps;
    uint32_t pad0;
    uint32_t pad1;
  };
  // See the K5 note: these two must agree with ocudu_mmse_ta.metal field for field.
  static_assert(sizeof(mmse_ta_params) == 32, "mmse_ta_params must match the MSL declaration");
  mmse_ta_params params{size,
           nof_slices,
           stride,
           static_cast<float>(scs_hz),
           max_ta_samples,
           (max_ta_samples > 2u) ? 5u : 3u,
           0u,
           0u};

  // float2 per sample, as the DFT engine leaves it.
  const NSUInteger slices_bytes = static_cast<NSUInteger>(size) * nof_slices * 2u * sizeof(float);
  id<MTLBuffer>    in_buf       = e->wrap_shared(slices, slices_bytes);
  id<MTLBuffer>    out_buf       = e->wrap_shared(&e->ta_out, sizeof(float));
  if (in_buf == nil || out_buf == nil) {
if (getenv("OCUDU_CE_TA_CHECK") != nullptr) {
    fprintf(stderr, "[ta_profile] wrap failed: in=%p out=%p bytes=%lu\n",
            (__bridge const void*)in_buf, (__bridge const void*)out_buf,
            static_cast<unsigned long>(slices_bytes));
  }
    return false;
  }

  // A command buffer of its own, committed and waited here, on the engine's own queue. It
  // deliberately does NOT join the lane's burst: the input this reduces is the DFT engine's output,
  // and that engine owns the FRONT-END queue while this one is the BACK-END queue - command buffers
  // of different queues have no ordering between them, so a profile encoded into this queue could run
  // before the transform that produces its input. Joining the burst is therefore part of the
  // integration, not of this entry point: it needs the transform and the reduction on ONE queue.
  // This entry builds its own command buffer without begin_stage(), so it has to close a held
  // extraction buffer itself (see begin_stage(): a held buffer is only ever adopted by the weights
  // entry point, and anything else must commit it FIRST or the extraction's dispatches would land
  // after work that depends on them).
  (void)close_held_buffer(e);
  id<MTLCommandBuffer>         cb  = [e->queue commandBuffer];
  id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
  if (enc == nil) {
    return false;
  }
  [enc setComputePipelineState:e->ta_pipe];
  [enc setBuffer:in_buf offset:0 atIndex:0];
  [enc setBuffer:out_buf offset:0 atIndex:1];
  [enc setBytes:&params length:sizeof(params) atIndex:2];
  // One threadgroup: the profile is a per-hop reduction, and its size is the kernel's own constant.
  [enc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
  [enc endEncoding];
  [cb commit];
  [cb waitUntilCompleted];
  if ((cb.status != MTLCommandBufferStatusCompleted) || (cb.error != nil)) {
if (getenv("OCUDU_CE_TA_CHECK") != nullptr) {
    fprintf(stderr, "[ta_profile] cb status=%ld err=%s\n",
            static_cast<long>(cb.status),
            (cb.error != nil) ? cb.error.localizedDescription.UTF8String : "none");
  }
    return false;
  }
  ta_seconds = e->ta_out;
  return !std::isnan(ta_seconds);
}

/// Must match mmse_scatter_params in ocudu_mmse_pilots.metal.
struct mmse_scatter_params_t {
  uint32_t nof_layers;
  uint32_t nof_symb;
  uint32_t nof_pilots;
  uint32_t npf;
  uint32_t pilot_base;
  uint32_t n_blk_slots;
  uint32_t n_blk_real;
  uint32_t Ls;
  float    inv_beta;
};
static_assert(sizeof(mmse_scatter_params_t) == 36, "mmse_scatter_params_t must match mmse_scatter_params");

/// \brief Encodes the pilot scatter (glue #2) into \p enc: the device writes the engine's y slots
/// out of the least-squares pilots K0-a already produced on the device.
///
/// \param[in] y_buf      The MTLBuffer the CALLER (run_async / encode_weights_only) already bound
///                       for its own y argument. It must be that very object, not a second wrap of
///                       the same memory: Metal relates two dispatches only through the resource
///                       object they bind, so binding the group's destination through its own wrap
///                       left the scatter unordered with respect to K2's read of it - measured, the
///                       merged tail group came out of the host memset (zeros) and the split tail
///                       out of a previous submission's leftovers, while the standard group - whose
///                       base pointer equals the caller's, hence the same object - was correct.
///                       One buffer, many offsets.
/// \param[in] y_base     Start of \p y_buf, to turn the descriptor's absolute pointer into an
///                       offset into that buffer.
/// \param[in] y_buf_bytes Length \p y_buf was wrapped with.
///
/// The writes must be visible to K2's reads, and the two are separate dispatches even in the same
/// encoder, so a buffer barrier closes the encode - exactly as the correlation prefix does.
/// \return False when nothing was encoded; the caller must then not commit (the host staging was
///         skipped in favour of this write, so a silent failure would leave stale pilots in y).
static bool encode_scatter(mmse_engine_impl* e, stage_encoder& st,
                           const mmse_engine::pilots_scatter& s, mmse_engine_impl::mapped y_buf,
                           const float* y_base, std::size_t y_buf_bytes)
{
  if ((e->pilots_scatter_pipe == nil) || (s.lse == nullptr) || (s.y == nullptr) || (y_buf.buf == nil) ||
      (y_base == nullptr) || (s.lse_bytes == 0) || (s.nof_layers == 0) || (s.nof_symb == 0) ||
      (s.nof_pilots == 0) || (s.npf == 0) || (s.n_blk_slots == 0) || (s.n_blk_real == 0) ||
      (s.n_blk_real > s.n_blk_slots) || (s.Ls == 0) || (s.nof_symb * s.npf > s.Ls)) {
    mmse_stats_pilots_scatter_failure();
    return false;
  }
  // The descriptor addresses the group's slots absolutely; the engine binds the BATCH's y buffer,
  // so the group's position in it is the difference. A group outside that buffer is a caller bug
  // (it would write over another batch's slots), hence the bound check rather than a clamp.
  const std::ptrdiff_t y_off = reinterpret_cast<const char*>(s.y) - reinterpret_cast<const char*>(y_base);
  const std::size_t    y_len = static_cast<std::size_t>(s.nof_layers) * s.n_blk_slots * 2 * s.Ls * sizeof(float);
  if ((y_off < 0) || (static_cast<std::size_t>(y_off) + y_len > y_buf_bytes)) {
    mmse_stats_pilots_scatter_failure();
    return false;
  }
  // The binding is the mapping's OWN offset plus the group's position inside the batch: the y buffer
  // may be a view into a mapping the batch base started (see wrap()), and binding only y_off would
  // write the group's pilots that many bytes away from where the apply kernel reads them.
  const NSUInteger y_bind = y_buf.offset + static_cast<NSUInteger>(y_off);
  if ((y_bind + y_len) > y_buf.buf.length) {
    mmse_stats_pilots_scatter_failure();
    return false;
  }
  // The source (K0-a's own output) is wrapped as the engine wraps it there: same pointer, same
  // capacity, so the cache hands back the same object and the two stages stay related.
  mmse_engine_impl::mapped lse_buf = e->wrap(s.lse, s.lse_bytes);
  if (lse_buf.buf == nil) {
    mmse_stats_pilots_scatter_failure();
    return false;
  }

  mmse_scatter_params_t p{};
  p.nof_layers  = s.nof_layers;
  p.nof_symb    = s.nof_symb;
  p.nof_pilots  = s.nof_pilots;
  p.npf         = s.npf;
  p.pilot_base  = s.pilot_base;
  p.n_blk_slots = s.n_blk_slots;
  p.n_blk_real  = s.n_blk_real;
  p.Ls          = s.Ls;
  p.inv_beta    = s.inv_beta;

  id<MTLComputeCommandEncoder> enc = stage_pipeline(e, st, e->pilots_scatter_pipe);
  [enc setBuffer:lse_buf.buf offset:lse_buf.offset atIndex:0];
  [enc setBuffer:y_buf.buf offset:y_bind atIndex:1];
  [enc setBytes:&p length:sizeof(p) atIndex:2];
  [enc dispatchThreads:MTLSizeMake(s.Ls, s.n_blk_slots, s.nof_layers)
      threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
  // In burst mode the next stage's pipeline change inserts this barrier (see stage_pipeline()); the own
  // command buffer has no such tracking, hence the explicit one.
  if (!st.burst) {
    [enc memoryBarrierWithScope:MTLBarrierScopeBuffers];
  }
  mmse_stats_pilots_scatter();
  return true;
}

static bool encode_run(mmse_engine_impl*     e,
                       float*                a,
                       const float*          r_hp,
                       float*                w,
                       const float*          y,
                       float*                h,
                       unsigned              nout,
                       unsigned              L,
                       unsigned              nof_systems,
                       unsigned              nof_blocks,
                       const mmse_engine::reformat_stage* reformat,
                       const mmse_engine::corr_stage*     corr,
                       const mmse_engine::corr_stage*     corr_edge,
                       const mmse_engine::pilots_scatter* scatter,
                       unsigned              nof_scatter,
                       bool                  wait_for_completion)
{
  // At most one submission in flight: the previous one must have completed before the staging
  // buffers it was reading can be overwritten. NOT the extraction this stage may be about to adopt
  // (pilots_stage::hold_for_weights) - that one is this hop's own work, and closing it here would
  // commit it separately, which is exactly the submission this stage exists to merge away.
  {
    mmse_guard_timer guard(e->pending_cb != nil);
    (void)wait_pending_impl(e, /*close_held=*/false);
  }

  mmse_phase_timer phase(wait_for_completion ? "run" : "run_async");
  mmse_engine_impl::mapped a_buf  = e->wrap(a, static_cast<NSUInteger>(nof_systems) * L * L * sizeof(float));
  mmse_engine_impl::mapped rp_buf = e->wrap(r_hp, static_cast<NSUInteger>(nof_systems) * nout * L * sizeof(float));
  mmse_engine_impl::mapped w_buf  = e->wrap(w, static_cast<NSUInteger>(nof_systems) * nout * L * sizeof(float));
  // The length the engine BINDS y with, and therefore the extent the scatter's offsets are checked
  // against. wrap() may hand back a larger cached buffer; the batch's own slots are what matters.
  const std::size_t y_bytes_used = static_cast<std::size_t>(nof_systems) * nof_blocks * 2 * L * sizeof(float);
  mmse_engine_impl::mapped y_buf = e->wrap(y, static_cast<NSUInteger>(y_bytes_used));
  mmse_engine_impl::mapped h_buf  = e->wrap(h, static_cast<NSUInteger>(nof_systems) * nof_blocks * 2 * nout * sizeof(float));
  phase.wrapped();
  if (a_buf.buf == nil || rp_buf.buf == nil || w_buf.buf == nil || y_buf.buf == nil || h_buf.buf == nil) {
    return false;
  }

  struct mmse_weights_params {
    uint32_t nout;
    uint32_t L;
    uint32_t nof_systems;
  } wparams{nout, L, nof_systems};
  struct mmse_apply_params {
    uint32_t nout;
    uint32_t L;
    uint32_t nof_systems;
    uint32_t nof_blocks;
  } aparams{nout, L, nof_systems, nof_blocks};

  // One command buffer, three ordered dispatches (K1 -> K1b -> K2), single commit/wait - or, in \c burst
  // order, the same dispatches in the command buffer the deferred chain shares, committed and waited by
  // the lane (see set_lane_order()). This is the path the air interface takes on every deferred hop: in
  // \c host_wait order it carries the [mmse_time_sum] gpu_wait the fused lane removed, and in \c event
  // order it is the command buffer the lane burst waits for through the back-end stage fence. The four
  // standalone stages wired first (K0-a, K0-d, K1, K2) are the other command buffers of the same hop.
  const bool use_rl = (e->inv_rl_pipe != nil) && (std::getenv("OCUDU_INV_RL") != nullptr);
  // The first pipeline the stage will encode with: begin_stage() hands it to shared_burst::encoder() so
  // that the barrier ordering this stage after the previous one (K0-a's, in the fused lane) is inserted
  // there. It must be a REAL pipeline: the burst would be told nil otherwise.
  id<MTLComputePipelineState> first_pipe = use_rl ? e->inv_rl_pipe : e->inv_pipe;
  if ((corr != nullptr) || (corr_edge != nullptr)) {
    first_pipe = e->corr_a_pipe;
  }
  if ((nof_scatter != 0) && (e->pilots_scatter_pipe != nil)) {
    first_pipe = e->pilots_scatter_pipe;
  }
  stage_encoder st;
  bool          adopted_held = false;
  const bool    fuse         = (e->lane_order == ce_lane_order::burst) && !wait_for_completion;
  st                         = begin_weights_stage(e, first_pipe, fuse, &adopted_held);
  id<MTLComputeCommandEncoder> enc = st.enc;
  phase.created();
  if (enc == nil) {
    return false;
  }

  // Glue #2 (S-7f-5u): the pilot vectors of this batch, written by the DEVICE out of K0-a's output.
  // Encoded FIRST because K2 is the reader and nothing else in this buffer touches y: the host
  // deliberately did not stage these slots (the estimator skips its memcpy when it hands a
  // descriptor over), so a failure here has to abort the whole submission rather than commit a
  // buffer whose weights would read the previous hop's pilots.
  for (unsigned i = 0; i != nof_scatter; ++i) {
    if (!encode_scatter(e, st, scatter[i], y_buf, y, y_bytes_used)) {
      abandon_stage(e, st, adopted_held);
      return false;
    }
  }

  // K1b (right-looking) is EXPERIMENTAL and numerically wrong at the orders this path uses now:
  // in the device-correlation A/B at order 54 it produced an inverse ~1e8 times the correct one
  // (the blocked K1 gives 353.2758 against the host's 353.2786 - see ocudu_mmse_inv.metal). It
  // stays behind OCUDU_INV_RL=1 until its defect is found; the blocked kernel is the default.
  // K0-d as a PREFIX of this buffer (S-7f-5p): the device builds A and R_hp here, K1 inverts A in
  // place right after, and nothing on the host ever reads or writes either - which is exactly the
  // device-inversion route. Everything before this point in the buffer is what the engine does
  // anyway. A failure is caught at ENCODE time, before the commit, so the caller can fall back
  // without a half-submitted batch (see run_weights_only()'s identical prefix).
  if (corr != nullptr) {
    // The stage may cover FEWER systems than the batch (corr_stage::nof_systems): a merged batch
    // builds its standard group here while the edge group's slots belong to another geometry.
    const unsigned corr_systems = (corr->nof_systems != 0) ? corr->nof_systems : nof_systems;
    if (!encode_corr(e, st, *corr, corr_systems)) {
      abandon_stage(e, st, adopted_held);
      mmse_stats_corr_build_failure();
      return false;
    }
    // COUNT IT HERE TOO. The standalone entry point (build_correlation()) is what used to be the
    // only way to build these matrices, and it owns the counter's increment; moving the build into
    // this prefix took the work with it but left the counter behind, so a phone leg of a working
    // device build reported device_corr_builds=0 - the very number the acceptance criteria read.
    mmse_stats_corr_build();
    // Same encoder: the correlation writes must be visible to K1's reads. In burst mode the pipeline
    // change to K1 below inserts that barrier with the stage switch (see stage_pipeline()).
    if (!st.burst) {
      [enc memoryBarrierWithScope:MTLBarrierScopeBuffers];
    }
  }

  // The SECOND geometry of a merged batch (the edge block), in this same command buffer. Exactly what
  // the standard stage above is for, and the reason corr_stage::nof_systems exists: a merged batch
  // builds its standard group in one geometry and its edge group in another, and both land in the
  // standard group's slots with different block orders. Building the edge HERE instead of in a
  // standalone build_correlation() keeps the batch in ONE command buffer - one commit, one wait -
  // which is what the fused lane is: the standalone form added a command buffer on every hop that had
  // an edge, and the lane paid a commit/wait pair for it (cbs/lane 3.00 -> 3.28 on air).
  if (corr_edge != nullptr) {
    const unsigned corr_edge_systems = (corr_edge->nof_systems != 0) ? corr_edge->nof_systems : nof_systems;
    if (!encode_corr(e, st, *corr_edge, corr_edge_systems)) {
      abandon_stage(e, st, adopted_held);
      mmse_stats_corr_build_failure();
      return false;
    }
    mmse_stats_corr_build();
    if (!st.burst) {
      [enc memoryBarrierWithScope:MTLBarrierScopeBuffers];
    }
  }

  enc = stage_pipeline(e, st, use_rl ? e->inv_rl_pipe : e->inv_pipe);
  [enc setBuffer:a_buf.buf offset:a_buf.offset atIndex:0];
  [enc setBytes:&L length:sizeof(unsigned) atIndex:1];
  [enc setBytes:&nof_systems length:sizeof(unsigned) atIndex:2];
  // Same (column, row) threadgroup layout AND the same geometry as invert(): the pivot-column
  // elimination spreads over the block instead of one thread walking a whole row, and the row
  // dimension is what S-5a measured to matter most (see mmse_inv_threadgroup()). Hardcoding (32,4)
  // here kept the worst measured geometry on the default path.
  {
    unsigned tgx = 0;
    unsigned tgy = 0;
    mmse_inv_threadgroup(tgx, tgy);
    [enc dispatchThreadgroups:MTLSizeMake(nof_systems, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(tgx, tgy, 1)];
  }

  enc = stage_pipeline(e, st, e->weights_pipe);
  [enc setBuffer:rp_buf.buf offset:rp_buf.offset atIndex:0];
  [enc setBuffer:a_buf.buf offset:a_buf.offset atIndex:1];
  [enc setBuffer:w_buf.buf offset:w_buf.offset atIndex:2];
  [enc setBytes:&wparams length:sizeof(wparams) atIndex:3];
  // One thread per output element: nof_systems * ceil(nout * L / 128) threadgroups.
  {
    const NSUInteger w_tgs = (static_cast<NSUInteger>(nout) * static_cast<NSUInteger>(L) + 127) / 128;
    [enc dispatchThreadgroups:MTLSizeMake(nof_systems * w_tgs, 1, 1) threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
  }

  enc = stage_pipeline(e, st, e->apply_pipe);
  [enc setBuffer:w_buf.buf offset:w_buf.offset atIndex:0];
  [enc setBuffer:y_buf.buf offset:y_buf.offset atIndex:1];
  [enc setBuffer:h_buf.buf offset:h_buf.offset atIndex:2];
  [enc setBytes:&aparams length:sizeof(aparams) atIndex:3];
  [enc dispatchThreadgroups:MTLSizeMake(nof_blocks * nof_systems, 1, 1)
      threadsPerThreadgroup:MTLSizeMake(nout, 1, 1)];

  encode_reformat(st, e, h_buf, reformat, nout, nof_blocks);

  phase.encoded();
  if (wait_for_completion) {
    const bool ok = end_stage(e, st, true, WEIGHTS_STAGE);
    phase.committed();
    return ok;
  }
  // \c adopted_held decides whether this submission IS the lane's first command buffer: when it is, the
  // host-gap reading belongs on its commit rather than on an extraction commit of its own (see
  // end_stage_async()).
  const bool ok = end_stage_async(e, st, true, WEIGHTS_STAGE, /*mark_extraction_commit=*/adopted_held);
  phase.committed();
  return collect_async_stage(e, st, ok);
}

bool mmse_engine::run_async(float*       a,
                            const float* r_hp,
                            float*       w,
                            const float* y,
                            float*       h,
                            unsigned     nout,
                            unsigned     L,
                            unsigned     nof_systems,
                            unsigned     nof_blocks,
                            const reformat_stage* reformat,
                            const corr_stage*     corr,
                            const pilots_scatter* scatter,
                            unsigned              nof_scatter,
                            const corr_stage*     corr_edge)
{
  auto* e = static_cast<mmse_engine_impl*>(impl);
  if (e == nullptr || e->device == nil) {
    return false;
  }
  return encode_run(e,
                    a,
                    r_hp,
                    w,
                    y,
                    h,
                    nout,
                    L,
                    nof_systems,
                    nof_blocks,
                    reformat,
                    corr,
                    corr_edge,
                    scatter,
                    nof_scatter,
                    /*wait_for_completion=*/false);
}

void mmse_engine::set_lane_order(ce_lane_order order)
{
  auto* e = static_cast<mmse_engine_impl*>(impl);
  if (e != nullptr) {
    e->lane_order = order;
  }
}

ce_lane_order mmse_engine::lane_order() const
{
  const auto* e = static_cast<const mmse_engine_impl*>(impl);
  return (e != nullptr) ? e->lane_order : ce_lane_order::host_wait;
}

/// \param[in] close_held Whether a held extraction buffer (pilots_stage::hold_for_weights) is closed
///            here. True for every caller that is NOT the weights entry point about to adopt it: a held
///            buffer that nothing adopts still has to reach the GPU, so it is committed, waited for and
///            counted - which is how a hop whose weights never came still publishes its extraction, and
///            why the extraction's results are readable by whoever called this. False only for the
///            weights stage's own entry guard (encode_run(): it is about to adopt that very buffer), and
///            the guard's job there is the PREVIOUS hop's submission (\c pending_cb), which is what the
///            staging buffers it is about to overwrite must be free of.
static bool wait_pending_impl(mmse_engine_impl* e, bool close_held)
{
  if (e == nullptr) {
    return true;
  }
  if (close_held && !close_held_buffer(e)) {
    return false;
  }
  if (e->pending_cb == nil) {
    // Nothing of this engine's is outstanding. Two routes reach this: \c burst order, where the dispatches
    // live in the caller's command buffer and whose commit and wait belong to the lane, and the hops that
    // were already collected. The stages call this at their boundaries and must be told yes without a wait
    // - and the GPU-time probe has to say ZERO rather than repeat the last own-command-buffer measurement,
    // which is the very [mmse_time_sum] gpu_wait a shared burst removes.
    if (e != nullptr) {
      e->last_gpu_us = 0.0;
    }
    return true;
  }
  id<MTLCommandBuffer> cb = e->pending_cb;
  e->pending_cb            = nil;
  [cb waitUntilCompleted];
  mmse_stats_wait();

  if (cb.status != MTLCommandBufferStatusCompleted || cb.error != nil) {
    return false;
  }
  if (cb.GPUStartTime != 0 && cb.GPUEndTime != 0) {
    e->last_gpu_us = (cb.GPUEndTime - cb.GPUStartTime) * 1e6;
  }
  return true;
}

bool mmse_engine::wait_pending()
{
  return wait_pending_impl(static_cast<mmse_engine_impl*>(impl));
}

bool mmse_engine::has_pending() const
{
  const auto* e = static_cast<const mmse_engine_impl*>(impl);
  return (e != nullptr) && (e->pending_cb != nil);
}

bool mmse_engine::complete_fused_burst()
{
  // Commit and wait, in that order. Both are no-ops when the lane already did them: it commits the
  // command buffer the stages share, and wait_committed() then finds nothing outstanding. A stage
  // that completes BEFORE that commit is the case this exists for (see the header).
  (void)ocudu::metal::shared_burst::commit();
  return ocudu::metal::shared_burst::wait_committed();
}

bool mmse_engine::burst_is_open()
{
  return ocudu::metal::shared_burst::open();
}

unsigned mmse_engine::burst_dispatch_count()
{
  return ocudu::metal::shared_burst::size();
}

bool mmse_engine::burst_commit_and_wait()
{
  (void)ocudu::metal::shared_burst::commit();
  return ocudu::metal::shared_burst::wait_committed();
}

bool mmse_engine::lane_fence_selftest(bool& waited)
{
  id<MTLCommandQueue> queue = ocudu::metal::shared_queue::backend_queue();
  if (queue == nil) {
    return false;
  }
  id<MTLCommandBuffer> cb = [queue commandBuffer];
  if (cb == nil) {
    return false;
  }
  // The lane burst's own wait, on a command buffer that does nothing else: if it names a generation no
  // command buffer will ever signal, the wait below never returns.
  waited = ocudu::metal::shared_queue::backend_stage_wait(cb);
  [cb commit];
  [cb waitUntilCompleted];
  return (cb.status == MTLCommandBufferStatusCompleted) && (cb.error == nil);
}

uint64_t mmse_engine::lane_fence_generation()
{
  return ocudu::metal::shared_queue::backend_stage_generation();
}

uint64_t mmse_engine::lane_fence_nof_signals()
{
  return ocudu::metal::shared_queue::backend_stage_nof_signals();
}

uint64_t mmse_engine::lane_fence_nof_waits()
{
  return ocudu::metal::shared_queue::backend_stage_nof_waits();
}

uint64_t mmse_engine::lane_fence_nof_skipped_waits()
{
  return ocudu::metal::shared_queue::backend_stage_nof_skipped_waits();
}

namespace {
/// \brief Encodes and commits the weights-only pipeline (K1b -> K2 -> K3/K4) for the two entry points below.
/// \param[in] wait_for_completion True for run_weights_only(), false for run_weights_only_async().
bool encode_weights_only(mmse_engine_impl*                  e,
                         const float*                       a_inv,
                         const float*                       r_hp,
                         float*                             w,
                         const float*                       y,
                         float*                             h,
                         unsigned                           nout,
                         unsigned                           L,
                         unsigned                           nof_systems,
                         unsigned                           nof_blocks,
                         const mmse_engine::reformat_stage* reformat,
                         const mmse_engine::corr_stage*     corr,
                         const mmse_engine::pilots_scatter* scatter,
                         unsigned                           nof_scatter,
                         bool                               wait_for_completion);
} // namespace

bool mmse_engine::run_weights_only(const float* a_inv, const float* r_hp, float* w, const float* y, float* h,
                                 unsigned nout, unsigned L, unsigned nof_systems, unsigned nof_blocks,
                                 const reformat_stage* reformat, const corr_stage* corr,
                                 const pilots_scatter* scatter, unsigned nof_scatter)
{
  auto* e = static_cast<mmse_engine_impl*>(impl);
  if (e == nullptr || e->device == nil) {
    return false;
  }
  {
    mmse_guard_timer guard(e->pending_cb != nil);
    (void)wait_pending();
  }
  return encode_weights_only(
      e, a_inv, r_hp, w, y, h, nout, L, nof_systems, nof_blocks, reformat, corr, scatter, nof_scatter, true);
}

bool mmse_engine::run_weights_only_async(const float* a_inv, const float* r_hp, float* w, const float* y, float* h,
                                        unsigned nout, unsigned L, unsigned nof_systems, unsigned nof_blocks,
                                        const reformat_stage* reformat, const corr_stage* corr,
                                        const pilots_scatter* scatter, unsigned nof_scatter)
{
  auto* e = static_cast<mmse_engine_impl*>(impl);
  if (e == nullptr || e->device == nil) {
    return false;
  }
  // Only this engine's own outstanding submission has to complete first: the staging buffers this call is about
  // to overwrite are exactly the ones it was reading.
  {
    mmse_guard_timer guard(e->pending_cb != nil);
    (void)wait_pending();
  }
  return encode_weights_only(
      e, a_inv, r_hp, w, y, h, nout, L, nof_systems, nof_blocks, reformat, corr, scatter, nof_scatter, false);
}

namespace {
bool encode_weights_only(mmse_engine_impl*                  e,
                         const float*                       a_inv,
                         const float*                       r_hp,
                         float*                             w,
                         const float*                       y,
                         float*                             h,
                         unsigned                           nout,
                         unsigned                           L,
                         unsigned                           nof_systems,
                         unsigned                           nof_blocks,
                         const mmse_engine::reformat_stage* reformat,
                         const mmse_engine::corr_stage*     corr,
                         const mmse_engine::pilots_scatter* scatter,
                         unsigned                           nof_scatter,
                         bool                               wait_for_completion)
{
  mmse_phase_timer phase(wait_for_completion ? "run_weights_only" : "run_weights_only_async");
  mmse_engine_impl::mapped ai_buf = e->wrap(a_inv, static_cast<NSUInteger>(nof_systems) * L * L * sizeof(float));
  mmse_engine_impl::mapped rp_buf = e->wrap(r_hp, static_cast<NSUInteger>(nof_systems) * nout * L * sizeof(float));
  mmse_engine_impl::mapped w_buf  = e->wrap(w, static_cast<NSUInteger>(nof_systems) * nout * L * sizeof(float));
  // The length the engine BINDS y with, and therefore the extent the scatter's offsets are checked
  // against. wrap() may hand back a larger cached buffer; the batch's own slots are what matters.
  const std::size_t y_bytes_used = static_cast<std::size_t>(nof_systems) * nof_blocks * 2 * L * sizeof(float);
  mmse_engine_impl::mapped y_buf = e->wrap(y, static_cast<NSUInteger>(y_bytes_used));
  mmse_engine_impl::mapped h_buf  = e->wrap(h, static_cast<NSUInteger>(nof_systems) * nof_blocks * 2 * nout * sizeof(float));
  phase.wrapped();
  if (ai_buf.buf == nil || rp_buf.buf == nil || w_buf.buf == nil || y_buf.buf == nil || h_buf.buf == nil) {
    return false;
  }

  struct mmse_weights_params {
    uint32_t nout;
    uint32_t L;
    uint32_t nof_systems;
  } wparams{nout, L, nof_systems};
  struct mmse_apply_params {
    uint32_t nout;
    uint32_t L;
    uint32_t nof_systems;
    uint32_t nof_blocks;
  } aparams{nout, L, nof_systems, nof_blocks};

  // The same two shapes as run_async(): this stage's own command buffer (single commit and - for the
  // synchronous entry point - single wait), or, in \c burst order, the command buffer the deferred chain
  // shares. Only the ASYNC entry point may join the burst: a synchronous caller has to leave with its
  // results ready, and the engine can only promise that with a command buffer of its own.
  const bool                    burst_ok   = (e->lane_order == ce_lane_order::burst) && !wait_for_completion;
  id<MTLComputePipelineState>  first_pipe = e->inv_pipe;
  if (corr != nullptr) {
    first_pipe = e->corr_a_pipe;
  }
  if ((nof_scatter != 0) && (e->pilots_scatter_pipe != nil)) {
    first_pipe = e->pilots_scatter_pipe;
  }
  stage_encoder                st  = begin_stage(e, first_pipe, burst_ok, /*wait_for_extraction=*/true);
  id<MTLComputeCommandEncoder> enc = st.enc;
  phase.created();
  if (enc == nil) {
    return false;
  }

  // Glue #2 (S-7f-5u): the pilot vectors, written by the DEVICE out of K0-a's output. FIRST, because
  // the apply kernel below is their reader. The host skipped its own staging in favour of this write,
  // so a failure must abort the submission (nothing is committed, the caller falls back to its CPU
  // path) rather than let the weights read the previous hop's pilots.
  for (unsigned i = 0; i != nof_scatter; ++i) {
    if (!encode_scatter(e, st, scatter[i], y_buf, y, y_bytes_used)) {
      if (!st.burst) {
        [enc endEncoding];
      }
      return false;
    }
  }

  // K0-d prefix: the correlation matrices are built into their slots FIRST, in this same command
  // buffer - the weights below read them, and K1 (which follows, in this same buffer) turns the A
  // they wrote into the A^-1 the weights need. Building them in a command buffer of their own costs
  // a whole submission round trip (~70us measured), which is what made the device build look
  // unprofitable; riding this buffer is what makes it pay.
  //
  // NOTE: the estimator does NOT use this form. Its block order is 54, where K1's inverse carries a
  // 1.3e-5 relative error (measured: device 353.2758 against the host's 353.2786), enough to take a
  // 256QAM capture from 24 dB to -18 dB of SINR. It calls build_correlation() standalone and lets
  // the HOST invert the device-built matrices, then calls this entry point with corr == nullptr.
  // This prefix is kept for the orders (<= 36) where K1 is accurate, because that is the form that
  // keeps the whole batch on the device.
  if (corr != nullptr) {
    const unsigned corr_systems = (corr->nof_systems != 0) ? corr->nof_systems : nof_systems;
    if (!encode_corr(e, st, *corr, corr_systems)) {
      if (!st.burst) {
        [enc endEncoding];
      }
      return false;
    }
    // EXPERIMENT (OCUDU_CE_DEV_INVERT=1): invert the freshly built A in this same command buffer, so
    // the whole matrix path stays on the device - no host inversion, no standalone round trip. This
    // is the form that would make the device build a net win; it is OFF by default because the
    // float32 kernel's accuracy at this conditioning is not established (see the plan: on a real A
    // the element-wise relative error is 9.7e-1 against the host's 1.7e-1, and Metal has no double
    // to fall back on).
    if (std::getenv("OCUDU_CE_DEV_INVERT") != nullptr) {
      enc = stage_pipeline(e,
                           st,
                           ((e->inv_rl_pipe != nil) && (std::getenv("OCUDU_INV_RL") != nullptr)) ? e->inv_rl_pipe
                                                                                               : e->inv_pipe);
      [enc setBuffer:ai_buf.buf offset:ai_buf.offset atIndex:0];
      [enc setBytes:&L length:sizeof(unsigned) atIndex:1];
      [enc setBytes:&nof_systems length:sizeof(unsigned) atIndex:2];
      unsigned tgx = 0;
      unsigned tgy = 0;
      mmse_inv_threadgroup(tgx, tgy);
      [enc dispatchThreadgroups:MTLSizeMake(nof_systems, 1, 1)
          threadsPerThreadgroup:MTLSizeMake(tgx, tgy, 1)];
    }
  }

  enc = stage_pipeline(e, st, e->weights_pipe);
  [enc setBuffer:rp_buf.buf offset:rp_buf.offset atIndex:0];
  [enc setBuffer:ai_buf.buf offset:ai_buf.offset atIndex:1];
  [enc setBuffer:w_buf.buf offset:w_buf.offset atIndex:2];
  [enc setBytes:&wparams length:sizeof(wparams) atIndex:3];
  // One thread per output element: nof_systems * ceil(nout * L / 128) threadgroups.
  {
    const NSUInteger w_tgs = (static_cast<NSUInteger>(nout) * static_cast<NSUInteger>(L) + 127) / 128;
    [enc dispatchThreadgroups:MTLSizeMake(nof_systems * w_tgs, 1, 1) threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
  }

  enc = stage_pipeline(e, st, e->apply_pipe);
  [enc setBuffer:w_buf.buf offset:w_buf.offset atIndex:0];
  [enc setBuffer:y_buf.buf offset:y_buf.offset atIndex:1];
  [enc setBuffer:h_buf.buf offset:h_buf.offset atIndex:2];
  [enc setBytes:&aparams length:sizeof(aparams) atIndex:3];
  [enc dispatchThreadgroups:MTLSizeMake(nof_blocks * nof_systems, 1, 1)
      threadsPerThreadgroup:MTLSizeMake(nout, 1, 1)];

  encode_reformat(st, e, h_buf, reformat, nout, nof_blocks);

  phase.encoded();
  if (wait_for_completion) {
    const bool ok = end_stage(e, st, true, WEIGHTS_STAGE);
    phase.committed();
    return ok;
  }
  const bool ok = end_stage_async(e, st, true, WEIGHTS_STAGE);
  phase.committed();
  return collect_async_stage(e, st, ok);
}
} // namespace

bool mmse_engine::init_matrix_pipelines()
{
  auto* e = static_cast<mmse_engine_impl*>(impl);
  if (e == nullptr || e->device == nil || e->library == nil) {
    return false;
  }
  if (e->weights_matrix_pipe != nil && e->apply_matrix_pipe != nil) {
    return true;
  }
  return e->load_matrix_pipelines();
}

bool mmse_engine::run_nn(const float* a_inv, const float* r_hp, float* w, const float* qy, float* h, unsigned nout,
                         unsigned L, unsigned nof_systems, unsigned nof_blocks)
{
  {
    // The local engine pointer is declared below: has_pending() is the null-safe query.
    mmse_guard_timer guard(has_pending());
    (void)wait_pending();
  }
  auto* e = static_cast<mmse_engine_impl*>(impl);
  if (e == nullptr || e->device == nil || e->weights_matrix_pipe == nil || e->apply_matrix_pipe == nil) {
    return false;
  }
  // The kernels always run: any non-zero nout/L are zero-padded by the caller to the next
  // multiples of 8 (Np/Lp below) inside the staging buffers - see the *_matrix.metal
  // comments for the padding contract. The apply kernel truncates the output back to the
  // real nout, so h keeps the legacy [systems][blocks][2*nout] layout.
  if (nout == 0 || L == 0 || nof_systems == 0 || nof_blocks == 0) {
    return false;
  }
  const uint32_t Lp     = (L + 7u) & ~7u;    // ceil8(L): row stride of the w/r_hp/a_inv regions
  const uint32_t Np     = (nout + 7u) & ~7u; // ceil8(nout)
  const uint32_t nquads = (nof_blocks + 3u) / 4u; // 4 blocks per quad, tail quad may be partial

  mmse_engine_impl::mapped ai_buf = e->wrap(a_inv, static_cast<NSUInteger>(nof_systems) * Lp * Lp * sizeof(float));
  mmse_engine_impl::mapped rp_buf = e->wrap(r_hp, static_cast<NSUInteger>(nof_systems) * Np * Lp * sizeof(float));
  mmse_engine_impl::mapped w_buf  = e->wrap(w, static_cast<NSUInteger>(nof_systems) * Np * Lp * sizeof(float));
  mmse_engine_impl::mapped qy_buf = e->wrap(qy, static_cast<NSUInteger>(nof_systems) * nquads * Lp * 8 * sizeof(float));
  mmse_engine_impl::mapped h_buf  = e->wrap(h, static_cast<NSUInteger>(nof_systems) * nof_blocks * 2 * nout * sizeof(float));
  if (ai_buf.buf == nil || rp_buf.buf == nil || w_buf.buf == nil || qy_buf.buf == nil || h_buf.buf == nil) {
    return false;
  }

  // Parameter structs mirror the MSL constant structs of ocudu_mmse_*_matrix.metal
  // (the kernels receive the ACTUAL dims and derive the padded ones themselves).
  struct mmse_weights_matrix_params {
    uint32_t nout;
    uint32_t L;
    uint32_t nof_systems;
  } wm{nout, L, nof_systems};
  struct mmse_apply_matrix_params {
    uint32_t nout;
    uint32_t L;
    uint32_t nof_systems;
    uint32_t nof_blocks;
  } am{nout, L, nof_systems, nof_blocks};

  // One command buffer, two ordered dispatches (W = R_hp . A^-1, then h = W . Y).
  // One SIMD-group (32 threads) computes one 8x8 output tile in both kernels; the tile
  // grids are ceil(nout/8) x ceil(L/8), so any dims are covered by the zero padding.
  // This entry builds its own command buffer without begin_stage(), so it has to close a held
  // extraction buffer itself (see begin_stage(): a held buffer is only ever adopted by the weights
  // entry point, and anything else must commit it FIRST or the extraction's dispatches would land
  // after work that depends on them).
  (void)close_held_buffer(e);
  id<MTLCommandBuffer> cb = [e->queue commandBuffer];
  id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];

  [enc setComputePipelineState:e->weights_matrix_pipe];
  [enc setBuffer:rp_buf.buf offset:rp_buf.offset atIndex:0];
  [enc setBuffer:ai_buf.buf offset:ai_buf.offset atIndex:1];
  [enc setBuffer:w_buf.buf offset:w_buf.offset atIndex:2];
  [enc setBytes:&wm length:sizeof(wm) atIndex:3];
  {
    const NSUInteger w_tgs = static_cast<NSUInteger>(nof_systems) * (Np / 8) * (Lp / 8);
    [enc dispatchThreadgroups:MTLSizeMake(w_tgs, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
  }

  [enc setComputePipelineState:e->apply_matrix_pipe];
  [enc setBuffer:w_buf.buf offset:w_buf.offset atIndex:0];
  [enc setBuffer:qy_buf.buf offset:qy_buf.offset atIndex:1];
  [enc setBuffer:h_buf.buf offset:h_buf.offset atIndex:2];
  [enc setBytes:&am length:sizeof(am) atIndex:3];
  {
    const NSUInteger a_tgs = static_cast<NSUInteger>(nof_systems) * nquads * (Np / 8);
    [enc dispatchThreadgroups:MTLSizeMake(a_tgs, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
  }

  [enc endEncoding];
  // The GPU-time probe must be armed before commit (Metal asserts otherwise).
  ocudu::metal::shared_queue::arm_gpu_time(cb, ocudu::metal::shared_queue::queue_kind::back_end);
  [cb commit];
  mmse_stats_commit();
  gpu_lane_probe::register_commit(cb, gpu_lane_probe::stage::channel_estimator);
  [cb waitUntilCompleted];
  mmse_stats_wait();

  if (cb.status != MTLCommandBufferStatusCompleted || cb.error != nil) {
    return false;
  }
  if (cb.GPUStartTime != 0 && cb.GPUEndTime != 0) {
    e->last_gpu_us = (cb.GPUEndTime - cb.GPUStartTime) * 1e6;
  }
  return true;
}

bool mmse_engine::reserve_buffer(const void* ptr, std::size_t bytes)
{
  auto* e = static_cast<mmse_engine_impl*>(impl);
  if ((e == nullptr) || (e->device == nil) || (ptr == nullptr) || (bytes == 0)) {
    return false;
  }
  return e->wrap(ptr, static_cast<NSUInteger>(bytes)).buf != nil;
}

bool mmse_engine::reserve_shared_buffer(const void* ptr, std::size_t bytes)
{
  auto* e = static_cast<mmse_engine_impl*>(impl);
  if ((e == nullptr) || (e->device == nil) || (ptr == nullptr) || (bytes == 0)) {
    return false;
  }
  return e->wrap_shared(ptr, static_cast<NSUInteger>(bytes)) != nil;
}

double mmse_engine::last_gpu_wait_us() const
{
  const auto* e = static_cast<const mmse_engine_impl*>(impl);
  return e == nullptr ? 0.0 : e->last_gpu_us;
}

} // namespace metal
} // namespace ocudu
