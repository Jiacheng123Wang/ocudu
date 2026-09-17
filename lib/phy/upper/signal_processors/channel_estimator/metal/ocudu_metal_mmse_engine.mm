// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu_metal_mmse_engine.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "ocudu_metal_lane_probe.h"
#include "ocudu_metal_burst.h"
#include "ocudu_metal_queue.h"

#include "ocudu/ocudulog/ocudulog.h"
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
               "device_y_writes=%llu y_write_fail=%llu device_sigma2=%llu\n",
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
}
#else  // OCUDU_METAL_STATS
static void mmse_stats_commit() {}
static void mmse_stats_wait() {}
static void mmse_stats_corr_build() {}
static void mmse_stats_corr_build_failure() {}
static void mmse_stats_pilots_scatter() {}
static void mmse_stats_pilots_scatter_failure() {}
static void mmse_stats_pilots_sigma2() {}

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
  // Glue #2: the device writes the engine's pilot vectors out of K0-a's output (optional, same
  // metallib - a metallib without it simply keeps the host staging).
  id<MTLComputePipelineState>    pilots_scatter_pipe = nil;
  /// Submission of run_async() that has not been waited for yet (at most one, see the header).
  id<MTLCommandBuffer>           pending_cb    = nil;
  // metal_nn_mmse: simdgroup_matrix 8x8 pipelines (optional, loaded on demand).
  id<MTLComputePipelineState>    weights_matrix_pipe = nil;
  id<MTLComputePipelineState>    apply_matrix_pipe  = nil;
  std::unordered_map<const void*, std::pair<id<MTLBuffer>, NSUInteger>> buffer_cache;
  std::mutex                                     cache_mutex;   // compute() may run on executor threads
  double                                         last_gpu_us = 0.0;

  // All Metal objects are ARC-managed (the translation unit compiles with -fobjc-arc).
  ~mmse_engine_impl() = default;

  id<MTLBuffer> wrap(const void* ptr, NSUInteger bytes)
  {
    {
      std::lock_guard<std::mutex> lock(cache_mutex);
      auto it = buffer_cache.find(ptr);
      if (it != buffer_cache.end()) {
        if (bytes <= it->second.second) {
          return it->second.first;
        }
        // Larger request than the cached wrap: re-wrap instead of silently handing back a
        // too-short buffer (S-1 audit fix). The construction-time warm-up wraps the maximum
        // sizes first, so this should never happen on the packet path - but it must not
        // truncate the GPU's view if it does.
        ocudulog::fetch_basic_logger("PHY").warning(
            "MMSE engine: zero-copy cache hit with a larger request ({} > cached {}): re-wrapping the buffer",
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
      buffer_cache.emplace(ptr, std::make_pair(buf, bytes));
    }
    return buf;
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
    id<MTLBuffer> buf = metal::shared_queue::wrap_no_copy(device, ptr, static_cast<size_t>(bytes));
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

/// \brief Opens a stage: the shared burst when \p fuse, the stage's own command buffer otherwise.
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
                                 bool                        fuse)
{
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
static bool end_stage(mmse_engine_impl* e, stage_encoder& s, bool encoded)
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
  // The GPU-time probe must be armed before commit (Metal asserts otherwise).
  ocudu::metal::shared_queue::arm_gpu_time(s.cb, ocudu::metal::shared_queue::queue_kind::back_end);
  [s.cb commit];
  mmse_stats_commit();
  ocudu::metal::gpu_lane_probe::register_commit(s.cb, ocudu::metal::gpu_lane_probe::stage::channel_estimator);
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
static bool end_stage_async(mmse_engine_impl* e, stage_encoder& s, bool encoded)
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
  ocudu::metal::gpu_lane_probe::register_commit(s.cb, ocudu::metal::gpu_lane_probe::stage::channel_estimator);
  e->pending_cb = s.cb;
  return true;
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
static void encode_reformat(stage_encoder&                             s,
                            mmse_engine_impl*                          e,
                            id<MTLBuffer>                              h_buf,
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
      [enc setBuffer:h_buf offset:0 atIndex:0];
      [enc setBytes:reformat->offsets
             length:static_cast<NSUInteger>(reformat->nof_symbols + 1) * sizeof(uint32_t)
             atIndex:1];
      [enc setBuffer:dst_buf offset:0 atIndex:2];
      [enc setBytes:&rparams length:sizeof(rparams) atIndex:3];
      const NSUInteger nof_sub =
          static_cast<NSUInteger>(rparams.sc_tail_base) + (reformat->has_tail ? rparams.nf_tail : 0u);
      const NSUInteger nof_threads = nof_sub * reformat->nof_symbols * reformat->nof_layers;
      [enc dispatchThreads:MTLSizeMake(nof_threads, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    }
  }

  // K4 (optional): the noise variance the equalizer scales its soft bits with, reduced from the
  // same h - one threadgroup covers the hop. It reads the estimates at the pilot positions, hence
  // the barrier after K2 (K3 and K4 write and read disjoint buffers, so their order is free).
  const ocudu::metal::mmse_engine::reformat_stage::noise_stage_t& noise =
      (reformat != nullptr) ? reformat->noise : ocudu::metal::mmse_engine::reformat_stage::noise_stage_t{};
  static const bool k4_enabled = (std::getenv("OCUDU_CE_NO_K4") == nullptr);
  if (k4_enabled && (reformat != nullptr) && (e->noise_pipe != nil) && (noise.nv != nullptr) && (noise.pilots != nullptr) &&
      (noise.rx_pilots != nullptr) && (noise.symbol_start_epochs != nullptr) && (noise.npt != 0) &&
      (noise.npf != 0) && (noise.comb_size != 0) && (reformat->nof_layers != 0)) {
    const NSUInteger pilots_bytes =
        static_cast<NSUInteger>(noise.npt) * reformat->nof_layers * noise.npf * 2 * sizeof(float);
    const NSUInteger rx_bytes =
        static_cast<NSUInteger>(noise.npt) * noise.nof_cdm_groups * noise.npf * 2 * sizeof(float);
    id<MTLBuffer> pilots_buf = e->wrap(noise.pilots, pilots_bytes);
    id<MTLBuffer> rx_buf     = e->wrap(noise.rx_pilots, rx_bytes);
    id<MTLBuffer> nv_buf     = e->wrap_shared(noise.nv, sizeof(float));
    if (pilots_buf != nil && rx_buf != nil && nv_buf != nil) {
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
        uint32_t nof_dmrs_pilots;
        uint32_t nof_cdm;
        float    min_snr_power;
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
                noise.nof_dmrs_pilots,
                noise.nof_cdm,
                noise.min_snr_power};
      for (unsigned i = 0; i != 4; ++i) {
        nparams.dmrs_slots[i] = noise.dmrs_slots[i];
      }
      if (!s.burst) {
        [enc memoryBarrierWithScope:MTLBarrierScopeBuffers];
      }
      enc = stage_pipeline(e, s, e->noise_pipe);
      [enc setBuffer:h_buf offset:0 atIndex:0];
      [enc setBuffer:pilots_buf offset:0 atIndex:1];
      [enc setBuffer:rx_buf offset:0 atIndex:2];
      [enc setBuffer:nv_buf offset:0 atIndex:3];
      [enc setBytes:&nparams length:sizeof(nparams) atIndex:4];
      [enc setBytes:noise.symbol_start_epochs
             length:static_cast<NSUInteger>(ocudu::MAX_NSYMB_PER_SLOT) * sizeof(float)
             atIndex:5];
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
  id<MTLFunction> noise_fn = [e->library newFunctionWithName:@"mmse_noise"];
  if (noise_fn != nil) {
    e->noise_pipe = [e->device newComputePipelineStateWithFunction:noise_fn
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
};
static_assert(sizeof(mmse_pilots_params_t) == 104, "mmse_pilots_params_t must match mmse_pilots_params");

/// Must match mmse_sigma2_params in ocudu_mmse_pilots.metal.
struct mmse_sigma2_params_t {
  uint32_t nof_dmrs_symb;
  uint32_t nof_layers;
  uint32_t nof_pilots;
  uint32_t nof_v_pilots;
  uint32_t filter_len;
  uint32_t nof_cdm;
  uint32_t compensate_cfo;
  float    beta;
  float    inv_beta;
  uint32_t dmrs_symb[4];
};
static_assert(sizeof(mmse_sigma2_params_t) == 52, "mmse_sigma2_params_t must match mmse_sigma2_params");

bool mmse_engine::build_pilots_lse(const pilots_stage& s)
{
  auto* e = static_cast<mmse_engine_impl*>(impl);
  if ((e == nullptr) || (e->device == nil) || (e->pilots_lse_pipe == nil) || (e->pilots_cfo_pipe == nil) ||
      (e->pilots_apply_pipe == nil)) {
    return false;
  }
  if ((s.grid == nullptr) || (s.grid_bytes == 0) || (s.ref == nullptr) || (s.epochs == nullptr) ||
      (s.lse == nullptr) || (s.cfo == nullptr) || (s.nof_dmrs_symb == 0) || (s.nof_dmrs_symb > 4) ||
      (s.nof_layers == 0) || (s.nof_pilots == 0) || (s.ncomb == 0) || (s.nof_prb == 0)) {
    return false;
  }

  const NSUInteger pilots = static_cast<NSUInteger>(s.nof_dmrs_symb) * s.nof_layers * s.nof_pilots;

  id<MTLBuffer> grid_buf = e->wrap(s.grid, s.grid_bytes);
  // Whole allocations, never the per-hop length: the wrap cache is pointer-keyed and a request
  // larger than its cached entry forces a re-map (see pilots_stage::buf_bytes).
  id<MTLBuffer> ref_buf  = e->wrap(s.ref, (s.buf_bytes != 0) ? s.buf_bytes : pilots * 2 * sizeof(float));
  id<MTLBuffer> lse_buf  = e->wrap(s.lse, (s.buf_bytes != 0) ? s.buf_bytes : pilots * 2 * sizeof(float));
  id<MTLBuffer> cfo_buf  = e->wrap(s.cfo, sizeof(float));
  id<MTLBuffer> ep_buf   = e->wrap(s.epochs, MAX_NSYMB_PER_SLOT * sizeof(float));
  if ((grid_buf == nil) || (ref_buf == nil) || (lse_buf == nil) || (cfo_buf == nil) || (ep_buf == nil)) {
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
  id<MTLBuffer> smoothed_buf = nil;
  id<MTLBuffer> filt_buf     = nil;
  id<MTLBuffer> rx_buf       = nil;
  id<MTLBuffer> sigma2_buf   = nil;
  if (sigma2_ok) {
    smoothed_buf = e->wrap(s.smoothed, (s.buf_bytes != 0) ? s.buf_bytes : pilots * 2 * sizeof(float));
    filt_buf     = e->wrap(s.fd_filter,
                           (s.fd_filter_bytes != 0) ? s.fd_filter_bytes : s.fd_filter_len * sizeof(float));
    rx_buf       = e->wrap(s.rx_pilots, s.rx_bytes);
    // Two floats: [0] the noise variance, [1] the pilots' power sum (see mmse_pilots_power). Wrapped
    // with its full length here and nowhere else, so the cache never sees a larger request later.
    sigma2_buf   = e->wrap(s.sigma2, 2 * sizeof(float));
    if ((smoothed_buf == nil) || (filt_buf == nil) || (rx_buf == nil) || (sigma2_buf == nil)) {
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

  // NOT fused, deliberately: this stage's outputs are read by the HOST inside the same hop - the CFO
  // (gpu_ls_cfo), the noise variance and the pilots' power (gpu_ls_sigma2) feed the statistics and the
  // noise reformat that this very hop still encodes, and the LS check reads the LSE - so the command
  // buffer has to be committed and waited here (see begin_stage()).
  stage_encoder                st  = begin_stage(e, e->pilots_lse_pipe, /*fuse=*/false);
  id<MTLCommandBuffer>         cb  = st.cb;
  id<MTLComputeCommandEncoder> enc = st.enc;

  [enc setComputePipelineState:e->pilots_lse_pipe];
  [enc setBuffer:grid_buf offset:0 atIndex:0];
  [enc setBuffer:ref_buf offset:0 atIndex:1];
  [enc setBuffer:lse_buf offset:0 atIndex:2];
  [enc setBytes:&p length:sizeof(p) atIndex:3];
  [enc dispatchThreads:MTLSizeMake(s.nof_pilots, s.nof_dmrs_symb * s.nof_layers, 1)
      threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];

  [enc setComputePipelineState:e->pilots_cfo_pipe];
  [enc setBuffer:lse_buf offset:0 atIndex:0];
  [enc setBuffer:ep_buf offset:0 atIndex:1];
  [enc setBuffer:cfo_buf offset:0 atIndex:2];
  [enc setBytes:&p length:sizeof(p) atIndex:3];
  [enc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];

  [enc setComputePipelineState:e->pilots_apply_pipe];
  [enc setBuffer:lse_buf offset:0 atIndex:0];
  [enc setBuffer:cfo_buf offset:0 atIndex:1];
  [enc setBuffer:ep_buf offset:0 atIndex:2];
  [enc setBytes:&p length:sizeof(p) atIndex:3];
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
    q.compensate_cfo = (s.compensate_cfo ? 1u : 0u);
    q.beta           = s.beta;
    q.inv_beta       = s.inv_beta;
    for (unsigned k = 0; k != 4; ++k) {
      q.dmrs_symb[k] = s.dmrs_symb[k];
    }
    // The CFO phasors use the estimate THIS command buffer just produced (cfo_buf): the host's own
    // estimate is not known yet, and the two agree to the precision the pilots do.
    [enc setComputePipelineState:e->pilots_smooth_pipe];
    [enc setBuffer:lse_buf offset:0 atIndex:0];
    [enc setBuffer:smoothed_buf offset:0 atIndex:1];
    [enc setBuffer:filt_buf offset:0 atIndex:2];
    [enc setBytes:&q length:sizeof(q) atIndex:3];
    // 128 = mmse_smooth_tg_size in ocudu_mmse_pilots.metal (the kernel strides its walk by it).
    [enc dispatchThreadgroups:MTLSizeMake(s.nof_dmrs_symb * s.nof_layers, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
    [enc memoryBarrierWithScope:MTLBarrierScopeBuffers];

    [enc setComputePipelineState:e->pilots_sigma2_pipe];
    [enc setBuffer:smoothed_buf offset:0 atIndex:0];
    [enc setBuffer:ref_buf offset:0 atIndex:1];
    [enc setBuffer:rx_buf offset:0 atIndex:2];
    [enc setBuffer:ep_buf offset:0 atIndex:3];
    [enc setBuffer:cfo_buf offset:0 atIndex:4];
    [enc setBuffer:sigma2_buf offset:0 atIndex:5];
    [enc setBytes:&q length:sizeof(q) atIndex:6];
    // 256 = mmse_sigma2_tg_size in ocudu_mmse_pilots.metal (its reduction tree is written for it).
    [enc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];

    // The pilots' mean power rides the same block: out[1] of the same buffer the sigma2 kernel wrote
    // out[0] of, so the host reads both scalars after the wait (see mmse_pilots_power).
    if (e->pilots_power_pipe != nil) {
      [enc memoryBarrierWithScope:MTLBarrierScopeBuffers];
      [enc setComputePipelineState:e->pilots_power_pipe];
      [enc setBuffer:lse_buf offset:0 atIndex:0];
      [enc setBuffer:sigma2_buf offset:0 atIndex:1];
      [enc setBytes:&q length:sizeof(q) atIndex:2];
      [enc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
    }
  }


  return end_stage(e, st, true);
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
  uint32_t dmrs_slots[4];
  uint32_t pilot_re[12];
};

// The kernel side asserts the same size: a field added on one side only would silently shift every
// field after it (the struct crosses the boundary as opaque setBytes bytes).
static_assert(sizeof(mmse_corr_params_t) == 124, "mmse_corr_params_t must match mmse_corr_params");

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
  const NSUInteger a_per_sys   = static_cast<NSUInteger>(c.l) * c.l;
  const NSUInteger rhp_per_sys = static_cast<NSUInteger>(nout) * c.l;
  // The batch is mapped by the SYSTEM stride, not by the packed per-system size: with a slot stride
  // wider than the block order the last system reaches past nof_systems * a_per_sys. A zero leaves
  // the packed spacing (the single-geometry case).
  const NSUInteger a_sys   = (c.a_sys_stride != 0) ? c.a_sys_stride : a_per_sys;
  const NSUInteger r_sys   = (c.r_sys_stride != 0) ? c.r_sys_stride : rhp_per_sys;
  const NSUInteger a_bytes = (static_cast<NSUInteger>(nof_systems - 1) * a_sys + a_per_sys) * sizeof(float);
  const NSUInteger rhp_bytes =
      (static_cast<NSUInteger>(nof_systems - 1) * r_sys + rhp_per_sys) * sizeof(float);

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
  id<MTLBuffer> a_buf   = e->wrap(c.a, a_bytes);
  id<MTLBuffer> rhp_buf = e->wrap(c.r_hp, rhp_bytes);
  if ((a_buf == nil) || (rhp_buf == nil)) {
    return false;
  }

  // Two pipelines in a row: in burst mode each switch goes through the stage's pipeline selection, so
  // the barrier that orders the A build against K1 (and the R_hp build against K2) is the burst's.
  id<MTLComputeCommandEncoder> enc = stage_pipeline(e, s, e->corr_a_pipe);
  [enc setBuffer:a_buf offset:0 atIndex:0];
  [enc setBytes:&p length:sizeof(p) atIndex:1];
  [enc dispatchThreads:MTLSizeMake(a_per_sys, nof_systems, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];

  enc = stage_pipeline(e, s, e->corr_rhp_pipe);
  [enc setBuffer:rhp_buf offset:0 atIndex:0];
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
  id<MTLBuffer>    a_buf = e->wrap(a, bytes);
  if (a_buf == nil) {
    return false;
  }
  const auto t_wrap1 = std::chrono::steady_clock::now();

  stage_encoder                st  = begin_stage(e, e->inv_pipe, /*fuse=*/false);
  id<MTLCommandBuffer>         cb  = st.cb;
  const auto t_cb1 = std::chrono::steady_clock::now();
  id<MTLComputeCommandEncoder> enc = st.enc;
  [enc setComputePipelineState:e->inv_pipe];
  [enc setBuffer:a_buf offset:0 atIndex:0];
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

  id<MTLBuffer> w_buf = e->wrap(w, static_cast<NSUInteger>(nof_systems) * nout * L * sizeof(float));
  id<MTLBuffer> y_buf = e->wrap(y, static_cast<NSUInteger>(nof_systems) * nof_blocks * 2 * L * sizeof(float));
  id<MTLBuffer> h_buf = e->wrap(h, static_cast<NSUInteger>(nof_systems) * nof_blocks * 2 * nout * sizeof(float));
  if (w_buf == nil || y_buf == nil || h_buf == nil) {
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
  [enc setBuffer:w_buf offset:0 atIndex:0];
  [enc setBuffer:y_buf offset:0 atIndex:1];
  [enc setBuffer:h_buf offset:0 atIndex:2];
  [enc setBytes:&params length:sizeof(params) atIndex:3];
  [enc dispatchThreadgroups:MTLSizeMake(nof_blocks * nof_systems, 1, 1)
      threadsPerThreadgroup:MTLSizeMake(nout, 1, 1)];

  return end_stage(e, st, true);
}

/// \brief Waits for the engine's own outstanding submission (the body of mmse_engine::wait_pending()).
///
/// A free function because the encode helpers below are file statics (they serve two public entry points
/// each): the wait belongs to the engine, not to the caller that happens to hold the public object.
static bool wait_pending_impl(mmse_engine_impl* e);

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
                           const mmse_engine::pilots_scatter& s, id<MTLBuffer> y_buf,
                           const float* y_base, std::size_t y_buf_bytes)
{
  if ((e->pilots_scatter_pipe == nil) || (s.lse == nullptr) || (s.y == nullptr) || (y_buf == nil) ||
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
  // The source (K0-a's own output) is wrapped as the engine wraps it there: same pointer, same
  // capacity, so the cache hands back the same object and the two stages stay related.
  id<MTLBuffer> lse_buf = e->wrap(s.lse, s.lse_bytes);
  if (lse_buf == nil) {
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
  [enc setBuffer:lse_buf offset:0 atIndex:0];
  [enc setBuffer:y_buf offset:static_cast<NSUInteger>(y_off) atIndex:1];
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
                       const mmse_engine::pilots_scatter* scatter,
                       unsigned              nof_scatter,
                       bool                  wait_for_completion)
{
  // At most one submission in flight: the previous one must have completed before the staging
  // buffers it was reading can be overwritten.
  {
    mmse_guard_timer guard(e->pending_cb != nil);
    (void)wait_pending_impl(e);
  }

  mmse_phase_timer phase(wait_for_completion ? "run" : "run_async");
  id<MTLBuffer> a_buf  = e->wrap(a, static_cast<NSUInteger>(nof_systems) * L * L * sizeof(float));
  id<MTLBuffer> rp_buf = e->wrap(r_hp, static_cast<NSUInteger>(nof_systems) * nout * L * sizeof(float));
  id<MTLBuffer> w_buf  = e->wrap(w, static_cast<NSUInteger>(nof_systems) * nout * L * sizeof(float));
  // The length the engine BINDS y with, and therefore the extent the scatter's offsets are checked
  // against. wrap() may hand back a larger cached buffer; the batch's own slots are what matters.
  const std::size_t y_bytes_used = static_cast<std::size_t>(nof_systems) * nof_blocks * 2 * L * sizeof(float);
  id<MTLBuffer>     y_buf        = e->wrap(y, static_cast<NSUInteger>(y_bytes_used));
  id<MTLBuffer> h_buf  = e->wrap(h, static_cast<NSUInteger>(nof_systems) * nof_blocks * 2 * nout * sizeof(float));
  phase.wrapped();
  if (a_buf == nil || rp_buf == nil || w_buf == nil || y_buf == nil || h_buf == nil) {
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
  if (corr != nullptr) {
    first_pipe = e->corr_a_pipe;
  }
  if ((nof_scatter != 0) && (e->pilots_scatter_pipe != nil)) {
    first_pipe = e->pilots_scatter_pipe;
  }
  stage_encoder st = begin_stage(e, first_pipe, /*fuse=*/(e->lane_order == ce_lane_order::burst) && !wait_for_completion);
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
      if (!st.burst) {
        [st.enc endEncoding];
      }
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
      if (!st.burst) {
        [st.enc endEncoding];
      }
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

  enc = stage_pipeline(e, st, use_rl ? e->inv_rl_pipe : e->inv_pipe);
  [enc setBuffer:a_buf offset:0 atIndex:0];
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
  [enc setBuffer:rp_buf offset:0 atIndex:0];
  [enc setBuffer:a_buf offset:0 atIndex:1];
  [enc setBuffer:w_buf offset:0 atIndex:2];
  [enc setBytes:&wparams length:sizeof(wparams) atIndex:3];
  // One thread per output element: nof_systems * ceil(nout * L / 128) threadgroups.
  {
    const NSUInteger w_tgs = (static_cast<NSUInteger>(nout) * static_cast<NSUInteger>(L) + 127) / 128;
    [enc dispatchThreadgroups:MTLSizeMake(nof_systems * w_tgs, 1, 1) threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
  }

  enc = stage_pipeline(e, st, e->apply_pipe);
  [enc setBuffer:w_buf offset:0 atIndex:0];
  [enc setBuffer:y_buf offset:0 atIndex:1];
  [enc setBuffer:h_buf offset:0 atIndex:2];
  [enc setBytes:&aparams length:sizeof(aparams) atIndex:3];
  [enc dispatchThreadgroups:MTLSizeMake(nof_blocks * nof_systems, 1, 1)
      threadsPerThreadgroup:MTLSizeMake(nout, 1, 1)];

  encode_reformat(st, e, h_buf, reformat, nout, nof_blocks);

  phase.encoded();
  if (wait_for_completion) {
    const bool ok = end_stage(e, st, true);
    phase.committed();
    return ok;
  }
  const bool ok = end_stage_async(e, st, true);
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
                            unsigned              nof_scatter)
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

static bool wait_pending_impl(mmse_engine_impl* e)
{
  if ((e == nullptr) || (e->pending_cb == nil)) {
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
  id<MTLBuffer> ai_buf = e->wrap(a_inv, static_cast<NSUInteger>(nof_systems) * L * L * sizeof(float));
  id<MTLBuffer> rp_buf = e->wrap(r_hp, static_cast<NSUInteger>(nof_systems) * nout * L * sizeof(float));
  id<MTLBuffer> w_buf  = e->wrap(w, static_cast<NSUInteger>(nof_systems) * nout * L * sizeof(float));
  // The length the engine BINDS y with, and therefore the extent the scatter's offsets are checked
  // against. wrap() may hand back a larger cached buffer; the batch's own slots are what matters.
  const std::size_t y_bytes_used = static_cast<std::size_t>(nof_systems) * nof_blocks * 2 * L * sizeof(float);
  id<MTLBuffer>     y_buf        = e->wrap(y, static_cast<NSUInteger>(y_bytes_used));
  id<MTLBuffer> h_buf  = e->wrap(h, static_cast<NSUInteger>(nof_systems) * nof_blocks * 2 * nout * sizeof(float));
  phase.wrapped();
  if (ai_buf == nil || rp_buf == nil || w_buf == nil || y_buf == nil || h_buf == nil) {
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
  stage_encoder                st  = begin_stage(e, first_pipe, burst_ok);
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
      [enc setBuffer:ai_buf offset:0 atIndex:0];
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
  [enc setBuffer:rp_buf offset:0 atIndex:0];
  [enc setBuffer:ai_buf offset:0 atIndex:1];
  [enc setBuffer:w_buf offset:0 atIndex:2];
  [enc setBytes:&wparams length:sizeof(wparams) atIndex:3];
  // One thread per output element: nof_systems * ceil(nout * L / 128) threadgroups.
  {
    const NSUInteger w_tgs = (static_cast<NSUInteger>(nout) * static_cast<NSUInteger>(L) + 127) / 128;
    [enc dispatchThreadgroups:MTLSizeMake(nof_systems * w_tgs, 1, 1) threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
  }

  enc = stage_pipeline(e, st, e->apply_pipe);
  [enc setBuffer:w_buf offset:0 atIndex:0];
  [enc setBuffer:y_buf offset:0 atIndex:1];
  [enc setBuffer:h_buf offset:0 atIndex:2];
  [enc setBytes:&aparams length:sizeof(aparams) atIndex:3];
  [enc dispatchThreadgroups:MTLSizeMake(nof_blocks * nof_systems, 1, 1)
      threadsPerThreadgroup:MTLSizeMake(nout, 1, 1)];

  encode_reformat(st, e, h_buf, reformat, nout, nof_blocks);

  phase.encoded();
  if (wait_for_completion) {
    const bool ok = end_stage(e, st, true);
    phase.committed();
    return ok;
  }
  const bool ok = end_stage_async(e, st, true);
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

  id<MTLBuffer> ai_buf = e->wrap(a_inv, static_cast<NSUInteger>(nof_systems) * Lp * Lp * sizeof(float));
  id<MTLBuffer> rp_buf = e->wrap(r_hp, static_cast<NSUInteger>(nof_systems) * Np * Lp * sizeof(float));
  id<MTLBuffer> w_buf  = e->wrap(w, static_cast<NSUInteger>(nof_systems) * Np * Lp * sizeof(float));
  id<MTLBuffer> qy_buf = e->wrap(qy, static_cast<NSUInteger>(nof_systems) * nquads * Lp * 8 * sizeof(float));
  id<MTLBuffer> h_buf  = e->wrap(h, static_cast<NSUInteger>(nof_systems) * nof_blocks * 2 * nout * sizeof(float));
  if (ai_buf == nil || rp_buf == nil || w_buf == nil || qy_buf == nil || h_buf == nil) {
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
  id<MTLCommandBuffer> cb = [e->queue commandBuffer];
  id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];

  [enc setComputePipelineState:e->weights_matrix_pipe];
  [enc setBuffer:rp_buf offset:0 atIndex:0];
  [enc setBuffer:ai_buf offset:0 atIndex:1];
  [enc setBuffer:w_buf offset:0 atIndex:2];
  [enc setBytes:&wm length:sizeof(wm) atIndex:3];
  {
    const NSUInteger w_tgs = static_cast<NSUInteger>(nof_systems) * (Np / 8) * (Lp / 8);
    [enc dispatchThreadgroups:MTLSizeMake(w_tgs, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
  }

  [enc setComputePipelineState:e->apply_matrix_pipe];
  [enc setBuffer:w_buf offset:0 atIndex:0];
  [enc setBuffer:qy_buf offset:0 atIndex:1];
  [enc setBuffer:h_buf offset:0 atIndex:2];
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
  return e->wrap(ptr, static_cast<NSUInteger>(bytes)) != nil;
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
