// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu_metal_mmse_engine.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

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
};

static mmse_stats_t& mmse_stats()
{
  static mmse_stats_t s;
  return s;
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
               "guard_mean=%.1fus guard_max=%.1fus\n",
               static_cast<unsigned long long>(s.commits.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.waits.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.in_flight_max.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(hits),
               static_cast<unsigned long long>(s.guard_calls.load(std::memory_order_relaxed)),
               (hits != 0) ? (static_cast<double>(wait) / static_cast<double>(hits) / 1e3) : 0.0,
               static_cast<double>(s.guard_wait_max_ns.load(std::memory_order_relaxed)) / 1e3);
}
#else  // OCUDU_METAL_STATS
static void mmse_stats_commit() {}
static void mmse_stats_wait() {}

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
  id<MTLDevice>                  device      = nil;
  id<MTLCommandQueue>            queue       = nil;
  id<MTLLibrary>                 library     = nil;
  id<MTLComputePipelineState>    inv_pipe    = nil;
  id<MTLComputePipelineState>    weights_pipe = nil;
  id<MTLComputePipelineState>    apply_pipe  = nil;
  // K3: per-symbol, mask-compressed cbf16 estimates for the equalizer (optional, loaded on demand).
  id<MTLComputePipelineState>    reformat_pipe = nil;
  // K4: the equalizer's noise variance, reduced on the device (optional, same metallib).
  id<MTLComputePipelineState>    noise_pipe    = nil;
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

// Appends the K3 gather (the equalizer's per-symbol estimates) to an encoder that has just run
// K2 over h. Shared by run() and run_weights_only() so both inversion paths produce it.
static void encode_reformat(id<MTLComputeCommandEncoder>              enc,
                            mmse_engine_impl*                          e,
                            id<MTLBuffer>                              h_buf,
                            const ocudu::metal::mmse_engine::reformat_stage* reformat,
                            unsigned                                   nout,
                            unsigned                                   nof_blocks)
{
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
      // rely on its in-order execution).
      [enc memoryBarrierWithScope:MTLBarrierScopeBuffers];
      [enc setComputePipelineState:e->reformat_pipe];
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
      [enc memoryBarrierWithScope:MTLBarrierScopeBuffers];
      [enc setComputePipelineState:e->noise_pipe];
      [enc setBuffer:h_buf offset:0 atIndex:0];
      [enc setBuffer:pilots_buf offset:0 atIndex:1];
      [enc setBuffer:rx_buf offset:0 atIndex:2];
      [enc setBuffer:nv_buf offset:0 atIndex:3];
      [enc setBytes:&nparams length:sizeof(nparams) atIndex:4];
      [enc setBytes:noise.symbol_start_epochs
             length:static_cast<NSUInteger>(ocudu::MAX_NSYMB_PER_SLOT) * sizeof(float)
             atIndex:5];
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
  // ARC-managed; no explicit release.
  return e->inv_pipe != nil && e->weights_pipe != nil && e->apply_pipe != nil;
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
  if (n > 36) {
    // The K1 kernel uses fixed-size threadgroup memory for 36x36; larger systems must use
    // the CPU inversion (the hot path does that anyway).
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

  id<MTLCommandBuffer> cb = [e->queue commandBuffer];
  const auto t_cb1 = std::chrono::steady_clock::now();
  id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
  [enc setComputePipelineState:e->inv_pipe];
  [enc setBuffer:a_buf offset:0 atIndex:0];
  [enc setBytes:&n length:sizeof(unsigned) atIndex:1];
  [enc setBytes:&nof_systems length:sizeof(unsigned) atIndex:2];
  // One threadgroup per system, laid out as (column, row) so that the elimination of a pivot
  // column spreads over the whole block (see ocudu_mmse_inv.metal).
  {
    const char* env = std::getenv("OCUDU_INV_TGX");
    const unsigned tgx = (env != nullptr) ? static_cast<unsigned>(std::strtoul(env, nullptr, 10)) : 32;
    const char* envy = std::getenv("OCUDU_INV_TGY");
    const unsigned tgy = (envy != nullptr) ? static_cast<unsigned>(std::strtoul(envy, nullptr, 10)) : 4;
    [enc dispatchThreadgroups:MTLSizeMake(nof_systems, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(tgx, tgy, 1)];
  }
  [enc endEncoding];
  [cb commit];
  mmse_stats_commit();
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

  id<MTLCommandBuffer> cb = [e->queue commandBuffer];
  id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
  [enc setComputePipelineState:e->apply_pipe];
  [enc setBuffer:w_buf offset:0 atIndex:0];
  [enc setBuffer:y_buf offset:0 atIndex:1];
  [enc setBuffer:h_buf offset:0 atIndex:2];
  [enc setBytes:&params length:sizeof(params) atIndex:3];
  [enc dispatchThreadgroups:MTLSizeMake(nof_blocks * nof_systems, 1, 1)
      threadsPerThreadgroup:MTLSizeMake(nout, 1, 1)];
  [enc endEncoding];
  [cb commit];
  mmse_stats_commit();
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

bool mmse_engine::run(float* a, const float* r_hp, float* w, const float* y, float* h, unsigned nout,
                      unsigned L, unsigned nof_systems, unsigned nof_blocks, const reformat_stage* reformat)
{
  return run_async(a, r_hp, w, y, h, nout, L, nof_systems, nof_blocks, reformat) && wait_pending();
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
                            const reformat_stage* reformat)
{
  auto* e = static_cast<mmse_engine_impl*>(impl);
  if (e == nullptr || e->device == nil) {
    return false;
  }
  // At most one submission in flight: the previous one must have completed before the staging
  // buffers it was reading can be overwritten.
  {
    mmse_guard_timer guard(e->pending_cb != nil);
    (void)wait_pending();
  }

  mmse_phase_timer phase("run_async");
  id<MTLBuffer> a_buf  = e->wrap(a, static_cast<NSUInteger>(nof_systems) * L * L * sizeof(float));
  id<MTLBuffer> rp_buf = e->wrap(r_hp, static_cast<NSUInteger>(nof_systems) * nout * L * sizeof(float));
  id<MTLBuffer> w_buf  = e->wrap(w, static_cast<NSUInteger>(nof_systems) * nout * L * sizeof(float));
  id<MTLBuffer> y_buf  = e->wrap(y, static_cast<NSUInteger>(nof_systems) * nof_blocks * 2 * L * sizeof(float));
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

  // One command buffer, three ordered dispatches (K1 -> K1b -> K2), single commit/wait.
  id<MTLCommandBuffer> cb = [e->queue commandBuffer];
  phase.created();
  id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];

  [enc setComputePipelineState:e->inv_pipe];
  [enc setBuffer:a_buf offset:0 atIndex:0];
  [enc setBytes:&L length:sizeof(unsigned) atIndex:1];
  [enc setBytes:&nof_systems length:sizeof(unsigned) atIndex:2];
  // Same (column, row) threadgroup layout as invert(): the pivot-column elimination spreads over
  // the block instead of one thread walking a whole row (ocudu_mmse_inv.metal). K1 is still
  // latency-bound (91.3 us for a single 36x36 system, 2 barriered pivot steps per column); S-5a
  // replaces the elimination with a blocked/simdgroup_matrix form before it becomes the default
  // inversion path - see the note in port_channel_estimator_metal_mmse_impl.cpp.
  [enc dispatchThreadgroups:MTLSizeMake(nof_systems, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 4, 1)];

  [enc setComputePipelineState:e->weights_pipe];
  [enc setBuffer:rp_buf offset:0 atIndex:0];
  [enc setBuffer:a_buf offset:0 atIndex:1];
  [enc setBuffer:w_buf offset:0 atIndex:2];
  [enc setBytes:&wparams length:sizeof(wparams) atIndex:3];
  // One thread per output element: nof_systems * ceil(nout * L / 128) threadgroups.
  {
    const NSUInteger w_tgs = (static_cast<NSUInteger>(nout) * static_cast<NSUInteger>(L) + 127) / 128;
    [enc dispatchThreadgroups:MTLSizeMake(nof_systems * w_tgs, 1, 1) threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
  }

  [enc setComputePipelineState:e->apply_pipe];
  [enc setBuffer:w_buf offset:0 atIndex:0];
  [enc setBuffer:y_buf offset:0 atIndex:1];
  [enc setBuffer:h_buf offset:0 atIndex:2];
  [enc setBytes:&aparams length:sizeof(aparams) atIndex:3];
  [enc dispatchThreadgroups:MTLSizeMake(nof_blocks * nof_systems, 1, 1)
      threadsPerThreadgroup:MTLSizeMake(nout, 1, 1)];

  encode_reformat(enc, e, h_buf, reformat, nout, nof_blocks);

  [enc endEncoding];
  phase.encoded();
  [cb commit];
  phase.committed();
  mmse_stats_commit();
  e->pending_cb = cb;
  return true;
}

bool mmse_engine::wait_pending()
{
  auto* e = static_cast<mmse_engine_impl*>(impl);
  if ((e == nullptr) || (e->pending_cb == nil)) {
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

bool mmse_engine::has_pending() const
{
  const auto* e = static_cast<const mmse_engine_impl*>(impl);
  return (e != nullptr) && (e->pending_cb != nil);
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
                         bool                               wait_for_completion);
} // namespace

bool mmse_engine::run_weights_only(const float* a_inv, const float* r_hp, float* w, const float* y, float* h,
                                 unsigned nout, unsigned L, unsigned nof_systems, unsigned nof_blocks,
                                 const reformat_stage* reformat)
{
  auto* e = static_cast<mmse_engine_impl*>(impl);
  if (e == nullptr || e->device == nil) {
    return false;
  }
  {
    mmse_guard_timer guard(e->pending_cb != nil);
    (void)wait_pending();
  }
  return encode_weights_only(e, a_inv, r_hp, w, y, h, nout, L, nof_systems, nof_blocks, reformat, true);
}

bool mmse_engine::run_weights_only_async(const float* a_inv, const float* r_hp, float* w, const float* y, float* h,
                                        unsigned nout, unsigned L, unsigned nof_systems, unsigned nof_blocks,
                                        const reformat_stage* reformat)
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
  return encode_weights_only(e, a_inv, r_hp, w, y, h, nout, L, nof_systems, nof_blocks, reformat, false);
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
                         bool                               wait_for_completion)
{
  mmse_phase_timer phase(wait_for_completion ? "run_weights_only" : "run_weights_only_async");
  id<MTLBuffer> ai_buf = e->wrap(a_inv, static_cast<NSUInteger>(nof_systems) * L * L * sizeof(float));
  id<MTLBuffer> rp_buf = e->wrap(r_hp, static_cast<NSUInteger>(nof_systems) * nout * L * sizeof(float));
  id<MTLBuffer> w_buf  = e->wrap(w, static_cast<NSUInteger>(nof_systems) * nout * L * sizeof(float));
  id<MTLBuffer> y_buf  = e->wrap(y, static_cast<NSUInteger>(nof_systems) * nof_blocks * 2 * L * sizeof(float));
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

  id<MTLCommandBuffer> cb = [e->queue commandBuffer];
  phase.created();
  id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];

  [enc setComputePipelineState:e->weights_pipe];
  [enc setBuffer:rp_buf offset:0 atIndex:0];
  [enc setBuffer:ai_buf offset:0 atIndex:1];
  [enc setBuffer:w_buf offset:0 atIndex:2];
  [enc setBytes:&wparams length:sizeof(wparams) atIndex:3];
  // One thread per output element: nof_systems * ceil(nout * L / 128) threadgroups.
  {
    const NSUInteger w_tgs = (static_cast<NSUInteger>(nout) * static_cast<NSUInteger>(L) + 127) / 128;
    [enc dispatchThreadgroups:MTLSizeMake(nof_systems * w_tgs, 1, 1) threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
  }

  [enc setComputePipelineState:e->apply_pipe];
  [enc setBuffer:w_buf offset:0 atIndex:0];
  [enc setBuffer:y_buf offset:0 atIndex:1];
  [enc setBuffer:h_buf offset:0 atIndex:2];
  [enc setBytes:&aparams length:sizeof(aparams) atIndex:3];
  [enc dispatchThreadgroups:MTLSizeMake(nof_blocks * nof_systems, 1, 1)
      threadsPerThreadgroup:MTLSizeMake(nout, 1, 1)];

  encode_reformat(enc, e, h_buf, reformat, nout, nof_blocks);

  [enc endEncoding];
  phase.encoded();
  [cb commit];
  phase.committed();
  mmse_stats_commit();
  if (!wait_for_completion) {
    e->pending_cb = cb;
    return true;
  }
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
  [cb commit];
  mmse_stats_commit();
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
