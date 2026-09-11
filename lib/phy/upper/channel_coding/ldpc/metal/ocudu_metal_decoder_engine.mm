// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// ocudu-side Metal engine for the layered normalized min-sum LDPC decoder
// (ocudu_nms_layered_decoder.metal). One command buffer per decode carries the
// whole Gauss-Seidel chain: per-layer CN/VN kernel pairs, a per-round final
// syndrome refresh, and the early-termination gate. The shaders are pre-compiled
// offline into .metallib files (xcrun metal/metallib) and loaded at runtime with
// newLibraryWithURL - no runtime shader compilation.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "ocudu_metal_decoder_engine.h"
#include "ocudu/ocudulog/ocudulog.h"

namespace ocudu {
namespace metal {

// ---- Process-wide dispatch/wait statistics (S-1 audit probe A2) ---------------------------
// Counted per command-buffer commit/wait across every engine of every algorithm family;
// reported at process exit when OCUDU_METAL_STATS is set. max_in_flight measures the
// cross-thread queue occupancy of the shared per-family command queue: with several pool
// threads committing on the same queue before waiting, it quantifies the submission-order
// serialization (audit bottleneck B7/B10).
struct decoder_stats_t {
  std::atomic<uint64_t> commits{0};
  std::atomic<uint64_t> waits{0};
  std::atomic<uint64_t> in_flight{0};
  std::atomic<uint64_t> in_flight_max{0};
};

static decoder_stats_t& decoder_stats()
{
  static decoder_stats_t s;
  return s;
}

static void decoder_stats_commit()
{
  decoder_stats_t& s = decoder_stats();
  s.commits.fetch_add(1, std::memory_order_relaxed);
  const uint64_t nf = s.in_flight.fetch_add(1, std::memory_order_acq_rel) + 1;
  uint64_t       prev = s.in_flight_max.load(std::memory_order_relaxed);
  while (nf > prev && !s.in_flight_max.compare_exchange_weak(prev, nf, std::memory_order_relaxed)) {
  }
}

static void decoder_stats_wait()
{
  decoder_stats_t& s = decoder_stats();
  s.waits.fetch_add(1, std::memory_order_relaxed);
  s.in_flight.fetch_sub(1, std::memory_order_acq_rel);
}

static void decoder_stats_report()
{
  if (std::getenv("OCUDU_METAL_STATS") == nullptr) {
    return;
  }
  const decoder_stats_t& s = decoder_stats();
  std::fprintf(stderr,
               "[metal_stats] ldpc_decoder commits=%llu waits=%llu max_in_flight=%llu\n",
               static_cast<unsigned long long>(s.commits.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.waits.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.in_flight_max.load(std::memory_order_relaxed)));
}

// Must match the DecodeCtrl struct in ocudu_nms_layered_decoder.metal (shader ABI).
// The async kernel uses the same 3-word layout as its AsyncCtrl (stop_flag,
// row_updates, actual_gens - field names differ, offsets are identical).
struct decode_ctrl_t {
  uint32_t error_count;     // unsatisfied check equations (current round) / async: stop_flag
  uint32_t early_terminate; // set by the ET gate when the syndrome converges / async: row_updates
  uint32_t actual_iters;    // rounds actually executed / async: actual_gens
  // LLS-only (the NMS shaders' DecodeCtrl stops after the first 3 words):
  uint32_t prev_error_count; // previous round's error_count (stall detection)
  uint32_t stall_counter;    // consecutive stall rounds
  uint32_t stall_flag;       // 1 = next round's update performs the hard multi-flip escape
};

// Must match the VNStats struct in ocudu_lls_decoder.metal (debug buffer layout).
struct vn_stats_t {
  uint32_t err_eq_cnt;
  uint32_t suspect_cnt;
  float    evid_sum;
  float    vn_total_cn;
  float    last_delta;
  float    current_llr;
};

struct engine_impl_t {
  id<MTLDevice>              device = nil;
  id<MTLCommandQueue>        queue  = nil;
  id<MTLComputePipelineState> p_nmsl_init = nil;
  id<MTLComputePipelineState> p_nmsl_convert = nil;
  id<MTLComputePipelineState> p_nmsl_cn = nil;
  id<MTLComputePipelineState> p_nmsl_final_syndrome = nil;
  id<MTLComputePipelineState> p_nmsl_et_gate = nil;
  id<MTLComputePipelineState> p_nmsl_persistent = nil; // layered_persistent: one dispatch per decode
  // Flooding pipelines (ocudu kernels).
  id<MTLComputePipelineState> p_nmsf_init = nil;
  id<MTLComputePipelineState> p_nmsf_convert = nil;
  id<MTLComputePipelineState> p_nmsf_cn = nil;
  id<MTLComputePipelineState> p_nmsf_vn = nil;
  // Asynchronous delta-BP pipelines (ocudu kernels).
  id<MTLComputePipelineState> p_nmsa_init = nil;
  id<MTLComputePipelineState> p_nmsa_delta_bp = nil;
  // LLS pipelines (SynchroPlus kernels, restored from the git history).
  id<MTLComputePipelineState> p_lls_init = nil;
  id<MTLComputePipelineState> p_lls_syndrome = nil;
  id<MTLComputePipelineState> p_lls_scan = nil;
  id<MTLComputePipelineState> p_lls_update = nil;

  // Zero-copy wrappers, cached by host pointer (the host buffers outlive the engine). The cached
  // length is stored alongside: a cache hit with a LARGER request re-wraps instead of silently
  // handing back a too-short buffer (S-1 audit fix; the previous size-blind cache relied on the
  // construction-time warm-up establishing the maximum size first).
  std::unordered_map<const void*, std::pair<id<MTLBuffer>, size_t>> buffer_cache;

  id<MTLBuffer> buf_h        = nil; // zero-copy (final syndrome refresh)
  id<MTLBuffer> buf_ctrl     = nil; // shared (GPU writes, CPU reads)
  id<MTLBuffer> buf_h_pred_bits = nil; // shared (one word per row; CPU recomputes the final syndrome)
  id<MTLBuffer> buf_row_start  = nil; // zero-copy CSR offsets (M_aligned + 1)
  id<MTLBuffer> buf_edge_vn    = nil; // zero-copy CSR edge VN indices
  id<MTLBuffer> buf_c2v        = nil; // private (per-edge messages, no_edges fp16)
  id<MTLBuffer> buf_llr_fp16   = nil; // shared (fp16 working LLRs; the conversion kernel writes, the hot loop mutates, the CPU reads back)
  // Flooding-only state.
  id<MTLBuffer> buf_ht         = nil; // zero-copy packed H^T
  id<MTLBuffer> buf_llr_chan   = nil; // shared (channel LLR copy, the VN sum anchor)
  id<MTLBuffer> buf_min1       = nil; // private (per check row)
  id<MTLBuffer> buf_min2       = nil; // private (per check row)
  id<MTLBuffer> buf_idx_min1   = nil; // private (per check row)
  // Async-only state.
  id<MTLBuffer> buf_p         = nil; // shared (uint32 Q16.16 posterior pool, atomic RMW + CPU readback)
  id<MTLBuffer> buf_r_old     = nil; // private (per-edge old messages, lane-owned)
  // LLS-only state (see PLAN.md 4.6: the fp16 LLRs are the zero-copy host buffer).
  id<MTLBuffer> buf_s_hard    = nil; // private (packed hard decisions, N/32 words)
  id<MTLBuffer> buf_h_pred    = nil; // shared (packed syndrome, M/32 words; CPU recomputes the final syndrome)
  id<MTLBuffer> buf_err_eq    = nil; // private (per-VN unsatisfied-row counters)
  id<MTLBuffer> buf_suspect   = nil; // private (per-VN suspect counters)
  id<MTLBuffer> buf_evidence  = nil; // private (per-VN evidence sums)
  id<MTLBuffer> buf_vn_total  = nil; // shared (per-VN column weights, CPU-computed, GPU read-only)
  id<MTLBuffer> buf_debug     = nil; // shared (VNStats debug layout)
  id<MTLBuffer> buf_cooldown  = nil; // private (per-VN 1-round flip-immunity flag, Phase 2)

  decoder_engine::layered_info layered_info{};

  decoder_engine::algo mode = decoder_engine::algo::layered;
  uint32_t n_aligned  = 0;
  uint32_t m_aligned  = 0;
  uint32_t n_h_chunks = 0;
  uint32_t h_pred_len = 0;
  uint32_t n_info     = 0;
  uint32_t z          = 0;
  float    factor     = 0.0f;
  float    beta       = 0.0f;
  bool     et_enabled = true;
  // LLS tuning parameters (PLAN.md 4.15); passed to the update kernel as one constant struct.
  decoder_engine::lls_params lls_params;
  // GPU-side duration of the last decode (0 when unavailable), for the
  // latency-benchmark breakdown: wall - gpu = CPU-side fixed overhead.
  double   last_gpu_us = 0.0;
  // 4KB-aligned dummy LLR buffer used by the init-time warm-up decode (engine lifetime;
  // the zero-copy wrapper cache keeps a valid entry for it).
  void* warmup_llr = nullptr;
};

namespace {

/// Resolves the pre-compiled shader library for one algorithm family: the
/// configure-time absolute path (compile definition) first, then a copy next to
/// the executable, then the working directory. Returns nil when no candidate
/// exists.
NSString* resolve_metallib_path(const char* macro_path, const char* lib_name)
{
  NSString* name = [NSString stringWithUTF8String:lib_name];
  NSMutableArray<NSString*>* candidates = [NSMutableArray arrayWithCapacity:3];
  if (macro_path != nullptr) {
    [candidates addObject:[NSString stringWithUTF8String:macro_path]];
  }
  NSArray<NSString*>* args = [[NSProcessInfo processInfo] arguments];
  if (args.count > 0) {
    [candidates addObject:[[args[0] stringByDeletingLastPathComponent] stringByAppendingPathComponent:name]];
  }
  [candidates addObject:[[[NSFileManager defaultManager] currentDirectoryPath] stringByAppendingPathComponent:name]];

  NSFileManager* fm = [NSFileManager defaultManager];
  for (NSString* path in candidates) {
    if ([fm fileExistsAtPath:path]) {
      return path;
    }
  }
  return nil;
}

id<MTLBuffer> zero_copy_buffer(engine_impl_t* engine, const void* ptr, size_t length)
{
  auto it = engine->buffer_cache.find(ptr);
  if (it != engine->buffer_cache.end()) {
    if (length <= it->second.second) {
      return it->second.first;
    }
    // Larger request than the cached wrap: re-wrap (the previous wrapper is replaced; ARC releases it).
    // This should never happen on the packet path (the warm-up establishes the maximum size), but it
    // must not silently truncate the GPU's view either.
    ocudulog::fetch_basic_logger("PHY").warning(
        "Metal LDPC: zero-copy cache hit with a larger request ({} > cached {}): re-wrapping the buffer",
        length,
        it->second.second);
  }
  size_t aligned = (length + 4095) & ~4095;
  id<MTLBuffer> buf =
      [engine->device newBufferWithBytesNoCopy:(void*)ptr
                                        length:aligned
                                       options:MTLResourceStorageModeShared
                                   deallocator:nil];
  if (buf == nil) {
    buf = [engine->device newBufferWithBytes:ptr length:length options:MTLResourceStorageModeShared];
  }
  engine->buffer_cache[ptr] = std::make_pair(buf, aligned);
  return buf;
}

/// Per-algorithm-family resources shared by every engine of that family: the Metal device, one command
/// queue and the compiled pipeline states. They are size-independent (the kernels receive their
/// dimensions via setBytes at dispatch time), so creating them per (BG, Z) slot - as the first version
/// did - reloaded the same .metallib and rebuilt the same pipelines for every new lifting size, paying
/// a several-ms stall on each new TB size during the run.
struct algo_resources_t {
  id<MTLDevice>                device                 = nil;
  id<MTLCommandQueue>          queue                  = nil;
  id<MTLComputePipelineState> p_nmsl_init            = nil;
  id<MTLComputePipelineState> p_nmsl_convert         = nil;
  id<MTLComputePipelineState> p_nmsl_cn              = nil;
  id<MTLComputePipelineState> p_nmsl_final_syndrome  = nil;
  id<MTLComputePipelineState> p_nmsl_et_gate         = nil;
  id<MTLComputePipelineState> p_nmsl_persistent      = nil;
  id<MTLComputePipelineState> p_nmsf_init            = nil;
  id<MTLComputePipelineState> p_nmsf_convert         = nil;
  id<MTLComputePipelineState> p_nmsf_cn              = nil;
  id<MTLComputePipelineState> p_nmsf_vn              = nil;
  id<MTLComputePipelineState> p_nmsa_init            = nil;
  id<MTLComputePipelineState> p_nmsa_delta_bp        = nil;
  // LLS family (SynchroPlus 4-kernel shader, ocudu_lls_decoder.metallib).
  id<MTLComputePipelineState> p_lls_init             = nil;
  id<MTLComputePipelineState> p_lls_syndrome         = nil;
  id<MTLComputePipelineState> p_lls_scan             = nil;
  id<MTLComputePipelineState> p_lls_update           = nil;
};

std::unordered_map<int, algo_resources_t>& algo_resources_cache()
{
  static std::unordered_map<int, algo_resources_t> cache;
  return cache;
}

std::mutex& algo_resources_mutex()
{
  static std::mutex m;
  return m;
}

/// Returns the shared resources of one algorithm family, creating them on first use (the .metallib is
/// loaded and the pipelines are compiled exactly once per family, logged once).
algo_resources_t* get_algo_resources(decoder_engine::algo mode)
{
  // Register the process-exit stats report exactly once (the counters live for the process).
  static std::once_flag stats_atexit_flag;
  std::call_once(stats_atexit_flag, []() { std::atexit(decoder_stats_report); });

  std::lock_guard<std::mutex> lock(algo_resources_mutex());
  auto&                      cache = algo_resources_cache();
  auto                       it    = cache.find(static_cast<int>(mode));
  if (it != cache.end()) {
    return &it->second;
  }

  algo_resources_t res;
  res.device = MTLCreateSystemDefaultDevice();
  if (res.device == nil) {
    ocudulog::fetch_basic_logger("PHY").error("Metal LDPC: no Metal device available");
    return nullptr;
  }
  res.queue = [res.device newCommandQueue];

  // Pre-compiled shader library per algorithm family (offline xcrun
  // metal/metallib, one .metallib per .metal source): loaded directly, so
  // engine init no longer pays the runtime shader compilation.
  const char* lib_name       = "ocudu_nms_layered_decoder.metallib";
  const char* lib_path_macro = nullptr;
#ifdef OCUDU_METAL_LAYERED_LIB_PATH
  lib_path_macro = OCUDU_METAL_LAYERED_LIB_PATH;
#endif
  if (mode == decoder_engine::algo::flooding) {
    lib_name       = "ocudu_nms_flooding_decoder.metallib";
    lib_path_macro = nullptr;
#ifdef OCUDU_METAL_FLOODING_LIB_PATH
    lib_path_macro = OCUDU_METAL_FLOODING_LIB_PATH;
#endif
  } else if (mode == decoder_engine::algo::async_delta) {
    lib_name       = "ocudu_nms_async_decoder.metallib";
    lib_path_macro = nullptr;
#ifdef OCUDU_METAL_ASYNC_LIB_PATH
    lib_path_macro = OCUDU_METAL_ASYNC_LIB_PATH;
#endif
  } else if (mode == decoder_engine::algo::lls) {
    lib_name       = "ocudu_lls_decoder.metallib";
    lib_path_macro = nullptr;
#ifdef OCUDU_METAL_LLS_LIB_PATH
    lib_path_macro = OCUDU_METAL_LLS_LIB_PATH;
#endif
  }
  NSString* lib_path = resolve_metallib_path(lib_path_macro, lib_name);
  if (lib_path == nil) {
    ocudulog::fetch_basic_logger("PHY").error(
        "Metal LDPC: pre-compiled shader library '{}' not found (searched the configure-time path, next "
        "to the executable, and the working directory)",
        lib_name);
    return nullptr;
  }
  NSError*       error   = nil;
  id<MTLLibrary> library = [res.device newLibraryWithURL:[NSURL fileURLWithPath:lib_path] error:&error];
  if (library == nil) {
    ocudulog::fetch_basic_logger("PHY").error("Metal LDPC: failed to load the pre-compiled shader library {}: {}",
                                              lib_path.UTF8String,
                                              error != nil ? error.localizedDescription.UTF8String : "nil error");
    return nullptr;
  }
  ocudulog::fetch_basic_logger("PHY").debug("Metal LDPC: loaded pre-compiled shader library {}",
                                            lib_path.UTF8String);

  auto make_pipeline = [&](id<MTLComputePipelineState> __strong* out, const char* name) -> bool {
    id<MTLFunction> fn = [library newFunctionWithName:[NSString stringWithUTF8String:name]];
    if (fn == nil) {
      ocudulog::fetch_basic_logger("PHY").error("Metal LDPC: kernel '{}' not found in the shader library", name);
      return false;
    }
    *out = [res.device newComputePipelineStateWithFunction:fn error:&error];
    if (*out == nil) {
      ocudulog::fetch_basic_logger("PHY").error("Metal LDPC: pipeline '{}' failed: {}",
                                                name,
                                                error != nil ? error.localizedDescription.UTF8String : "nil error");
      return false;
    }
    return true;
  };
  if (mode == decoder_engine::algo::flooding) {
    if (!make_pipeline(&res.p_nmsf_init, "nmsf_init") ||
        !make_pipeline(&res.p_nmsf_convert, "nmsf_i8_to_fp16") ||
        !make_pipeline(&res.p_nmsf_cn, "nmsf_cn_update") || !make_pipeline(&res.p_nmsf_vn, "nmsf_vn_update")) {
      return nullptr;
    }
  } else if (mode == decoder_engine::algo::layered_persistent) {
    if (!make_pipeline(&res.p_nmsl_persistent, "nmsl_persistent_decode")) {
      return nullptr;
    }
  } else if (mode == decoder_engine::algo::async_delta) {
    if (!make_pipeline(&res.p_nmsa_init, "nmsa_init") || !make_pipeline(&res.p_nmsa_delta_bp, "nmsa_delta_bp")) {
      return nullptr;
    }
  } else if (mode == decoder_engine::algo::lls) {
    if (!make_pipeline(&res.p_lls_init, "init_hard_decisions") ||
        !make_pipeline(&res.p_lls_syndrome, "compute_syndrome") ||
        !make_pipeline(&res.p_lls_scan, "cn_centric_scan") ||
        !make_pipeline(&res.p_lls_update, "update_llr_hpred")) {
      return nullptr;
    }
  } else {
    if (!make_pipeline(&res.p_nmsl_init, "nmsl_init") || !make_pipeline(&res.p_nmsl_convert, "nmsl_i8_to_fp16") ||
        !make_pipeline(&res.p_nmsl_cn, "nmsl_cn_update") ||
        !make_pipeline(&res.p_nmsl_final_syndrome, "nmsl_final_syndrome") ||
        !make_pipeline(&res.p_nmsl_et_gate, "nmsl_et_gate")) {
      return nullptr;
    }
  }

  auto [inserted, ok] = cache.emplace(static_cast<int>(mode), std::move(res));
  return ok ? &inserted->second : nullptr;
}

} // namespace

decoder_engine::~decoder_engine()
{
  engine_impl_t* engine = static_cast<engine_impl_t*>(impl);
  if (engine != nullptr) {
    // Release the zero-copy wrappers BEFORE freeing the warm-up buffer they reference.
    engine->buffer_cache.clear();
    std::free(engine->warmup_llr);
    delete engine;
    impl = nullptr;
  }
}

bool decoder_engine::init(uint32_t n_logical, uint32_t m_logical, float factor, float beta,
                          const uint32_t* h, const uint32_t* ht, const layered_info& layered,
                          algo mode, bool et_enabled, const lls_params* lls)
{
  engine_impl_t* engine = new engine_impl_t();
  impl                  = engine;

  engine->n_info     = n_logical - m_logical;
  engine->factor     = factor;
  engine->beta       = beta;
  engine->mode       = mode;
  engine->et_enabled = et_enabled;
  engine->layered_info = layered;
  engine->z          = layered.z;
  // LLS: the parameter struct replaces the bare alpha; without one the legacy
  // behavior is reproduced exactly (alpha = factor, everything else at default).
  engine->lls_params = lls != nullptr ? *lls : lls_params{};
  if (lls == nullptr) {
    engine->lls_params.alpha = factor;
  }

  engine->n_aligned  = ((n_logical + 31) / 32) * 32;
  engine->m_aligned  = ((m_logical + 31) / 32) * 32;
  engine->n_h_chunks = engine->n_aligned / 32;
  engine->h_pred_len = engine->m_aligned / 32;

  // Device, command queue, shader library and pipeline states are shared across every engine of the
  // same algorithm family (they are size-independent); the per-(BG, Z) part below is only the buffers.
  const algo_resources_t* res = get_algo_resources(mode);
  if (res == nullptr) {
    return false;
  }
  engine->device                = res->device;
  engine->queue                 = res->queue;
  engine->p_nmsl_init           = res->p_nmsl_init;
  engine->p_nmsl_convert        = res->p_nmsl_convert;
  engine->p_nmsl_cn             = res->p_nmsl_cn;
  engine->p_nmsl_final_syndrome = res->p_nmsl_final_syndrome;
  engine->p_nmsl_et_gate        = res->p_nmsl_et_gate;
  engine->p_nmsl_persistent     = res->p_nmsl_persistent;
  engine->p_nmsf_init           = res->p_nmsf_init;
  engine->p_nmsf_convert        = res->p_nmsf_convert;
  engine->p_nmsf_cn             = res->p_nmsf_cn;
  engine->p_nmsf_vn             = res->p_nmsf_vn;
  engine->p_nmsa_init           = res->p_nmsa_init;
  engine->p_nmsa_delta_bp       = res->p_nmsa_delta_bp;
  engine->p_lls_init            = res->p_lls_init;
  engine->p_lls_syndrome        = res->p_lls_syndrome;
  engine->p_lls_scan            = res->p_lls_scan;
  engine->p_lls_update          = res->p_lls_update;

  engine->buf_ctrl = [engine->device newBufferWithLength:sizeof(decode_ctrl_t)
                                                 options:MTLResourceStorageModeShared];

  // Shared (not private): the CPU recomputes the final syndrome after the kernels
  // complete (the gate clears ctrl->error_count at the end of every round).
  engine->buf_h_pred_bits = [engine->device newBufferWithLength:engine->m_aligned * sizeof(uint32_t)
                                                        options:MTLResourceStorageModeShared];

  // CSR edge layout (zero-copy) + per-edge messages + per-VN deltas.
  engine->buf_row_start = zero_copy_buffer(
      engine, engine->layered_info.row_start,
      static_cast<size_t>(engine->m_aligned + 1) * sizeof(uint32_t));
  engine->buf_edge_vn = zero_copy_buffer(
      engine, engine->layered_info.edge_vn,
      static_cast<size_t>(engine->layered_info.no_edges) * sizeof(uint32_t));
  engine->buf_llr_fp16 = [engine->device newBufferWithLength:engine->n_aligned * sizeof(uint16_t)
                                                     options:MTLResourceStorageModeShared];

  // Zero-copy wrap the host-side packed H matrix (4KB-aligned, owned by the caller).
  // The layered family computes the syndrome from the CSR edge lists (identical edge
  // sets), so H is only needed by the flooding / async / LLS kernels.
  const bool needs_h = (mode == algo::flooding) || (mode == algo::async_delta) || (mode == algo::lls);
  if (needs_h) {
    engine->buf_h = zero_copy_buffer(engine, h, static_cast<size_t>(engine->m_aligned) *
                                                   engine->n_h_chunks * sizeof(uint32_t));
  }

  if (mode == algo::lls) {
    // LLS buffers (see PLAN.md 4.6): the fp16 LLRs live in the zero-copy host buffer, s_hard packs
    // the hard decisions, h_pred holds the incrementally-maintained packed syndrome (shared, so the
    // CPU can recompute the final syndrome), the per-VN statistics are private, and vn_total_cn
    // holds the CPU-computed column weights (GPU read-only).
    engine->buf_ht = zero_copy_buffer(engine, ht, static_cast<size_t>(engine->n_aligned) *
                                                      engine->h_pred_len * sizeof(uint32_t));
    engine->buf_s_hard = [engine->device newBufferWithLength:engine->n_h_chunks * sizeof(uint32_t)
                                                     options:MTLResourceStorageModePrivate];
    engine->buf_h_pred = [engine->device newBufferWithLength:engine->h_pred_len * sizeof(uint32_t)
                                                     options:MTLResourceStorageModeShared];
    engine->buf_err_eq = [engine->device newBufferWithLength:engine->n_aligned * sizeof(uint32_t)
                                                     options:MTLResourceStorageModePrivate];
    engine->buf_suspect = [engine->device newBufferWithLength:engine->n_aligned * sizeof(uint32_t)
                                                      options:MTLResourceStorageModePrivate];
    engine->buf_evidence = [engine->device newBufferWithLength:engine->n_aligned * sizeof(float)
                                                       options:MTLResourceStorageModePrivate];
    engine->buf_vn_total = [engine->device newBufferWithLength:engine->n_aligned * sizeof(uint32_t)
                                                       options:MTLResourceStorageModeShared];
    engine->buf_debug = [engine->device newBufferWithLength:engine->n_aligned * sizeof(vn_stats_t)
                                                    options:MTLResourceStorageModeShared];
    engine->buf_cooldown = [engine->device newBufferWithLength:engine->n_aligned * sizeof(uint32_t)
                                                           options:MTLResourceStorageModePrivate];

    // Compute the column weights on the CPU (once per engine; the GPU reads them read-only).
    uint32_t* col_weights = static_cast<uint32_t*>(engine->buf_vn_total.contents);
    std::memset(col_weights, 0, static_cast<size_t>(engine->n_aligned) * sizeof(uint32_t));
    for (uint32_t r = 0; r != engine->m_aligned; ++r) {
      const uint32_t* row_base = h + r * engine->n_h_chunks;
      for (uint32_t c = 0; c != engine->n_h_chunks; ++c) {
        uint32_t mask     = row_base[c];
        uint32_t col_base = c * 32;
        while (mask != 0) {
          const uint32_t bit = static_cast<uint32_t>(__builtin_ctz(mask));
          if (col_base + bit < engine->n_aligned) {
            col_weights[col_base + bit]++;
          }
          mask &= (mask - 1);
        }
      }
    }
  } else if (mode == algo::flooding) {
    engine->buf_ht = zero_copy_buffer(engine, ht, static_cast<size_t>(engine->n_aligned) *
                                                      engine->h_pred_len * sizeof(uint32_t));
    engine->buf_llr_chan = [engine->device newBufferWithLength:engine->n_aligned * sizeof(uint16_t)
                                                       options:MTLResourceStorageModeShared];
    engine->buf_min1 = [engine->device newBufferWithLength:engine->m_aligned * sizeof(float)
                                                   options:MTLResourceStorageModePrivate];
    engine->buf_min2 = [engine->device newBufferWithLength:engine->m_aligned * sizeof(float)
                                                   options:MTLResourceStorageModePrivate];
    engine->buf_idx_min1 = [engine->device newBufferWithLength:engine->m_aligned * sizeof(uint32_t)
                                                       options:MTLResourceStorageModePrivate];
  } else if (mode == algo::async_delta) {
    engine->buf_p = [engine->device newBufferWithLength:engine->n_aligned * sizeof(uint32_t)
                                                options:MTLResourceStorageModeShared];
    engine->buf_r_old = [engine->device newBufferWithLength:engine->layered_info.no_edges * sizeof(float)
                                                    options:MTLResourceStorageModePrivate];
  } else {
    engine->buf_c2v = [engine->device newBufferWithLength:engine->layered_info.no_edges * sizeof(uint16_t)
                                                  options:MTLResourceStorageModePrivate];
  }

  // Warm-up dispatch: the first dispatch of each pipeline pays the Metal driver's lazy
  // compile / first command-buffer commit (~ms, CPU-side - inside the slot if left to the
  // first real decode). A 1-iteration dummy decode here moves that cost to the (lazy,
  // per-TB-size) slot construction. Same pattern as the MMSE channel-estimator engine.
  // The kernel JIT is per-process per pipeline: only the FIRST engine of a family needs
  // the warm-up decode; later engines of the same family skip it (their buffers are
  // wrapped on first use, which is microsecond-scale).
  {
    static std::atomic<int> family_warmed{0};
    const int              mode_bit = 1 << static_cast<int>(mode);
    if ((family_warmed.load(std::memory_order_acquire) & mode_bit) == 0) {
      if (::posix_memalign(&engine->warmup_llr, 4096,
                           static_cast<size_t>(engine->n_aligned) * sizeof(uint16_t)) == 0) {
        std::memset(engine->warmup_llr, 0, static_cast<size_t>(engine->n_aligned) * sizeof(uint16_t));
        // Outputs are skipped (nullptr hard-bits / error-count arguments).
        (void)decode(engine->warmup_llr, nullptr, /*max_iter=*/1, nullptr);
      }
      family_warmed.fetch_or(mode_bit, std::memory_order_release);
    }
  }

  return true;
}

int decoder_engine::decode(const void* in_fp16, uint8_t* out_bits, int max_iter, uint32_t* error_count_out)
{
  engine_impl_t* engine = static_cast<engine_impl_t*>(impl);
  const bool     is_persistent = engine != nullptr && engine->mode == decoder_engine::algo::layered_persistent;
  const bool     is_async      = engine != nullptr && engine->mode == decoder_engine::algo::async_delta;
  const bool     is_lls        = engine != nullptr && engine->mode == decoder_engine::algo::lls;
  if (engine == nullptr ||
      (is_lls       ? engine->p_lls_init == nil
       : is_persistent ? engine->p_nmsl_persistent == nil
        : is_async    ? engine->p_nmsa_delta_bp == nil
                      : (engine->mode == decoder_engine::algo::flooding ? engine->p_nmsf_init == nil
                                                                        : engine->p_nmsl_init == nil))) {
    return -1;
  }
  const bool is_flooding = engine->mode == decoder_engine::algo::flooding;
  // The LLS kernels consume fp16 LLRs in place (host-filled, zero-copy); the NMS kernels consume the
  // int8 buffer and convert to fp16 on the GPU.
  id<MTLBuffer> mtl_llr_i8 =
      zero_copy_buffer(engine, in_fp16, static_cast<size_t>(engine->n_aligned) *
                                            (is_lls ? sizeof(uint16_t) : sizeof(int8_t)));
  id<MTLCommandBuffer>        cmd_buf = [engine->queue commandBuffer];
  id<MTLComputeCommandEncoder> enc    = [cmd_buf computeCommandEncoder];

  if (is_lls) {
    // Stage A: hard-decision init + control-block and per-VN statistics reset.
    [enc setComputePipelineState:engine->p_lls_init];
    [enc setBuffer:mtl_llr_i8 offset:0 atIndex:0];
    [enc setBuffer:engine->buf_s_hard offset:0 atIndex:1];
    [enc setBuffer:engine->buf_ctrl offset:0 atIndex:2];
    [enc setBuffer:engine->buf_err_eq offset:0 atIndex:3];
    [enc setBuffer:engine->buf_suspect offset:0 atIndex:4];
    [enc setBuffer:engine->buf_evidence offset:0 atIndex:5];
    [enc setBuffer:engine->buf_cooldown offset:0 atIndex:6];
    [enc dispatchThreads:MTLSizeMake(engine->n_aligned, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];

    // Stage B: initial full syndrome computation (once; maintained incrementally afterwards).
    [enc setComputePipelineState:engine->p_lls_syndrome];
    [enc setBuffer:engine->buf_h offset:0 atIndex:0];
    [enc setBuffer:engine->buf_s_hard offset:0 atIndex:1];
    [enc setBuffer:engine->buf_h_pred offset:0 atIndex:2];
    [enc setBuffer:engine->buf_ctrl offset:0 atIndex:3];
    [enc setBytes:&engine->n_h_chunks length:sizeof(uint32_t) atIndex:4];
    [enc dispatchThreadgroups:MTLSizeMake(engine->h_pred_len, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];

    // Unrolled round loop, fully consumed by the GPU: exactly two dispatches per round (CN-centric
    // scan + VN update with the incremental HT-XOR syndrome maintenance), with the GPU-internal
    // early termination (DecodeCtrl flags; the remaining rounds' kernels return immediately).
    for (int it = 0; it < max_iter; ++it) {
      [enc setComputePipelineState:engine->p_lls_scan];
      [enc setBuffer:engine->buf_h offset:0 atIndex:0];
      [enc setBuffer:mtl_llr_i8 offset:0 atIndex:1];
      [enc setBuffer:engine->buf_h_pred offset:0 atIndex:2];
      [enc setBuffer:engine->buf_err_eq offset:0 atIndex:3];
      [enc setBuffer:engine->buf_suspect offset:0 atIndex:4];
      [enc setBuffer:engine->buf_evidence offset:0 atIndex:5];
      [enc setBytes:&engine->n_h_chunks length:sizeof(uint32_t) atIndex:6];
      [enc setBuffer:engine->buf_ctrl offset:0 atIndex:7];
      [enc setBytes:&engine->lls_params length:sizeof(decoder_engine::lls_params) atIndex:8];
      [enc setBuffer:engine->buf_cooldown offset:0 atIndex:9];
      [enc dispatchThreadgroups:MTLSizeMake(engine->m_aligned, 1, 1)
          threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];

      [enc setComputePipelineState:engine->p_lls_update];
      [enc setBuffer:mtl_llr_i8 offset:0 atIndex:0];
      [enc setBuffer:engine->buf_err_eq offset:0 atIndex:1];
      [enc setBuffer:engine->buf_suspect offset:0 atIndex:2];
      [enc setBuffer:engine->buf_evidence offset:0 atIndex:3];
      [enc setBuffer:engine->buf_vn_total offset:0 atIndex:4];
      [enc setBuffer:engine->buf_h_pred offset:0 atIndex:5];
      [enc setBuffer:engine->buf_ht offset:0 atIndex:6];
      [enc setBytes:&engine->h_pred_len length:sizeof(uint32_t) atIndex:7];
      [enc setBytes:&engine->lls_params length:sizeof(decoder_engine::lls_params) atIndex:8];
      [enc setBuffer:engine->buf_ctrl offset:0 atIndex:9];
      [enc setBuffer:engine->buf_debug offset:0 atIndex:10];
      [enc setBuffer:engine->buf_cooldown offset:0 atIndex:11];
      [enc dispatchThreads:MTLSizeMake(engine->n_aligned, 1, 1)
          threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
    }
  } else if (is_flooding) {
    // Stage A: reset the control block (the conversion prepares the LLRs).
    [enc setComputePipelineState:engine->p_nmsf_init];
    [enc setBuffer:engine->buf_ctrl offset:0 atIndex:0];
    [enc dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];

    // Static bindings, bound ONCE for the whole decode (disjoint index ranges:
    // CN 0-7, VN 10-20, conversion 21-23); the hot round loop contains ONLY
    // setComputePipelineState + dispatch calls - zero setBytes/setBuffer.
    [enc setBuffer:engine->buf_h offset:0 atIndex:0];
    [enc setBuffer:engine->buf_llr_fp16 offset:0 atIndex:1];
    [enc setBuffer:engine->buf_h_pred_bits offset:0 atIndex:2];
    [enc setBuffer:engine->buf_min1 offset:0 atIndex:3];
    [enc setBuffer:engine->buf_min2 offset:0 atIndex:4];
    [enc setBuffer:engine->buf_idx_min1 offset:0 atIndex:5];
    [enc setBuffer:engine->buf_ctrl offset:0 atIndex:6];
    [enc setBytes:&engine->n_h_chunks length:sizeof(uint32_t) atIndex:7];
    [enc setBuffer:engine->buf_llr_fp16 offset:0 atIndex:10];
    [enc setBuffer:engine->buf_llr_chan offset:0 atIndex:11];
    [enc setBuffer:engine->buf_min1 offset:0 atIndex:12];
    [enc setBuffer:engine->buf_min2 offset:0 atIndex:13];
    [enc setBuffer:engine->buf_idx_min1 offset:0 atIndex:14];
    [enc setBuffer:engine->buf_h_pred_bits offset:0 atIndex:15];
    [enc setBuffer:engine->buf_ht offset:0 atIndex:16];
    [enc setBytes:&engine->h_pred_len length:sizeof(uint32_t) atIndex:17];
    [enc setBytes:&engine->factor length:sizeof(float) atIndex:18];
    [enc setBytes:&engine->beta length:sizeof(float) atIndex:19];
    [enc setBuffer:engine->buf_ctrl offset:0 atIndex:20];
    [enc setBuffer:mtl_llr_i8 offset:0 atIndex:21];
    [enc setBuffer:engine->buf_llr_fp16 offset:0 atIndex:22];
    [enc setBuffer:engine->buf_llr_chan offset:0 atIndex:23];

    // One-time int8 -> fp16 conversion (working LLRs + channel LLR copy).
    [enc setComputePipelineState:engine->p_nmsf_convert];
    [enc dispatchThreads:MTLSizeMake(engine->n_aligned, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];

    // Unrolled round loop: exactly two dispatches per round (CN over all check
    // rows, VN over all VNs), with the GPU-internal ET fused into the kernels.
    for (int it = 0; it < max_iter; ++it) {
      [enc setComputePipelineState:engine->p_nmsf_cn];
      [enc dispatchThreadgroups:MTLSizeMake(engine->m_aligned, 1, 1)
          threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
      [enc setComputePipelineState:engine->p_nmsf_vn];
      [enc dispatchThreads:MTLSizeMake(engine->n_aligned, 1, 1)
          threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
    }
  } else if (is_persistent) {
  // Persistent layered decode: ONE resident threadgroup of 1024 threads runs
  // the kernel's prologue (ctrl reset, c2v zeroing, int8->fp16 conversion),
  // the (iteration, layer) loops, the syndrome refresh and the ET gate, all
  // serialized by threadgroup_barrier inside the kernel - the whole decode
  // is exactly ONE dispatch.
  [enc setComputePipelineState:engine->p_nmsl_persistent];
  [enc setBuffer:engine->buf_row_start offset:0 atIndex:0];
  [enc setBuffer:engine->buf_edge_vn offset:0 atIndex:1];
  [enc setBuffer:engine->buf_c2v offset:0 atIndex:2];
  [enc setBuffer:engine->buf_llr_fp16 offset:0 atIndex:3];
  [enc setBuffer:engine->buf_h_pred_bits offset:0 atIndex:4];
  [enc setBytes:&engine->z length:sizeof(uint32_t) atIndex:5];
  [enc setBytes:&engine->factor length:sizeof(float) atIndex:6];
  [enc setBytes:&engine->beta length:sizeof(float) atIndex:7];
  [enc setBuffer:engine->buf_ctrl offset:0 atIndex:8];
  const uint32_t n_layers = engine->layered_info.n_layers;
  [enc setBytes:&n_layers length:sizeof(uint32_t) atIndex:9];
  const uint32_t max_iter_u = static_cast<uint32_t>(max_iter);
  [enc setBytes:&max_iter_u length:sizeof(uint32_t) atIndex:10];
  const uint32_t no_edges = engine->layered_info.no_edges;
  [enc setBytes:&no_edges length:sizeof(uint32_t) atIndex:11];
  [enc setBytes:&engine->n_aligned length:sizeof(uint32_t) atIndex:12];
  [enc setBytes:&engine->m_aligned length:sizeof(uint32_t) atIndex:13];
  [enc setBuffer:mtl_llr_i8 offset:0 atIndex:14];
  [enc dispatchThreadgroups:MTLSizeMake(1, 1, 1)
      threadsPerThreadgroup:MTLSizeMake(1024, 1, 1)];
  } else if (is_async) {
  // Asynchronous delta-BP: TWO dispatches per decode - the init (ctrl reset,
  // int8 -> Q16.16 pool, R_old zeroing) and the barrier-free persistent grid
  // (one threadgroup per check row plus the syndrome poller), which loops
  // generations of residual-delta injection until the stop flag or the
  // max_iter generation bound.
  const uint32_t init_threads = engine->n_aligned > engine->layered_info.no_edges ? engine->n_aligned
                                                                                  : engine->layered_info.no_edges;
  [enc setComputePipelineState:engine->p_nmsa_init];
  [enc setBuffer:engine->buf_p offset:0 atIndex:0];
  [enc setBuffer:engine->buf_r_old offset:0 atIndex:1];
  [enc setBuffer:engine->buf_ctrl offset:0 atIndex:2];
  [enc setBuffer:mtl_llr_i8 offset:0 atIndex:3];
  [enc setBytes:&engine->n_aligned length:sizeof(uint32_t) atIndex:4];
  const uint32_t no_edges = engine->layered_info.no_edges;
  [enc setBytes:&no_edges length:sizeof(uint32_t) atIndex:5];
  [enc dispatchThreads:MTLSizeMake(init_threads, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];

  [enc setComputePipelineState:engine->p_nmsa_delta_bp];
  [enc setBuffer:engine->buf_p offset:0 atIndex:0];
  [enc setBuffer:engine->buf_r_old offset:0 atIndex:1];
  [enc setBuffer:engine->buf_ctrl offset:0 atIndex:2];
  [enc setBuffer:engine->buf_row_start offset:0 atIndex:3];
  [enc setBuffer:engine->buf_edge_vn offset:0 atIndex:4];
  [enc setBuffer:engine->buf_h_pred_bits offset:0 atIndex:5];
  [enc setBuffer:engine->buf_h offset:0 atIndex:6];
  [enc setBytes:&engine->n_h_chunks length:sizeof(uint32_t) atIndex:7];
  [enc setBytes:&engine->m_aligned length:sizeof(uint32_t) atIndex:8];
  [enc setBytes:&engine->factor length:sizeof(float) atIndex:9];
  [enc setBytes:&engine->beta length:sizeof(float) atIndex:10];
  const uint32_t max_iter_u = static_cast<uint32_t>(max_iter);
  [enc setBytes:&max_iter_u length:sizeof(uint32_t) atIndex:11];
  [enc dispatchThreadgroups:MTLSizeMake(engine->m_aligned + 1, 1, 1)
      threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
  } else {
  // Stage A: reset the control block and zero the per-edge messages.
  [enc setComputePipelineState:engine->p_nmsl_init];
  [enc setBuffer:engine->buf_c2v offset:0 atIndex:0];
  [enc setBuffer:engine->buf_ctrl offset:0 atIndex:1];
  [enc dispatchThreadgroups:MTLSizeMake((engine->layered_info.no_edges + 31) / 32, 1, 1)
      threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];

  // Static bindings, bound ONCE for the whole decode: the kernels use disjoint
  // argument index ranges (CN 0-8, final syndrome 10-14, ET gate 15) and the
  // encoder's binding table is stateful across setComputePipelineState calls,
  // so only the per-layer setBytes + dispatch remain inside the loops.
  // One-time int8 -> fp16 conversion (indices 16/17), then the hot loop.
  [enc setBuffer:mtl_llr_i8 offset:0 atIndex:16];
  [enc setBuffer:engine->buf_llr_fp16 offset:0 atIndex:17];
  // CN (fused CN+VN): 0-8.
  [enc setBuffer:engine->buf_row_start offset:0 atIndex:0];
  [enc setBuffer:engine->buf_edge_vn offset:0 atIndex:1];
  [enc setBuffer:engine->buf_c2v offset:0 atIndex:2];
  [enc setBuffer:engine->buf_llr_fp16 offset:0 atIndex:3];
  [enc setBuffer:engine->buf_h_pred_bits offset:0 atIndex:4];
  [enc setBytes:&engine->factor length:sizeof(float) atIndex:6];
  [enc setBytes:&engine->beta length:sizeof(float) atIndex:7];
  [enc setBuffer:engine->buf_ctrl offset:0 atIndex:8];
  // Final syndrome (CSR walk): 10-14.
  [enc setBuffer:engine->buf_row_start offset:0 atIndex:10];
  [enc setBuffer:engine->buf_edge_vn offset:0 atIndex:11];
  [enc setBuffer:engine->buf_llr_fp16 offset:0 atIndex:12];
  [enc setBuffer:engine->buf_h_pred_bits offset:0 atIndex:13];
  [enc setBuffer:engine->buf_ctrl offset:0 atIndex:14];
  // ET gate: 15.
  [enc setBuffer:engine->buf_ctrl offset:0 atIndex:15];

  // One-time int8 -> fp16 conversion (bindings 16/17 are already in place).
  [enc setComputePipelineState:engine->p_nmsl_convert];
  [enc dispatchThreads:MTLSizeMake(engine->n_aligned, 1, 1)
      threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];

  // Unrolled round loop, fully consumed by the GPU: per-layer fused CN+VN
  // dispatches (Gauss-Seidel chaining via the sequential command stream), a
  // per-round final syndrome refresh (also accumulates the unsatisfied-row
  // count), and the ET gate that stops the remaining rounds on a clean
  // syndrome. The pipeline is set once per round; per layer only the
  // layer_start setBytes and the dispatch remain.
  for (int it = 0; it < max_iter; ++it) {
    [enc setComputePipelineState:engine->p_nmsl_cn];
    for (uint32_t l = 0; l != engine->layered_info.n_layers; ++l) {
      const uint32_t layer_start = l * engine->z;
      [enc setBytes:&layer_start length:sizeof(uint32_t) atIndex:5];
      [enc dispatchThreadgroups:MTLSizeMake(engine->z, 1, 1)
          threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
    }
    [enc setComputePipelineState:engine->p_nmsl_final_syndrome];
    [enc dispatchThreadgroups:MTLSizeMake(engine->m_aligned, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
    if (engine->et_enabled) {
      [enc setComputePipelineState:engine->p_nmsl_et_gate];
      [enc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    }
  }
  }

  [enc endEncoding];
  [cmd_buf commit];
  decoder_stats_commit();
  [cmd_buf waitUntilCompleted];
  decoder_stats_wait();
  if (cmd_buf.status != MTLCommandBufferStatusCompleted) {
    ocudulog::fetch_basic_logger("PHY").error("Metal LDPC: command buffer failed with status {}",
                                              static_cast<unsigned long>(cmd_buf.status));
    return -1;
  }
  // GPU-side duration (Apple Silicon: valid for compute command buffers; 0 when unavailable).
  if (cmd_buf.GPUStartTime > 0.0 && cmd_buf.GPUEndTime > 0.0) {
    engine->last_gpu_us = (cmd_buf.GPUEndTime - cmd_buf.GPUStartTime) * 1e6;
  } else {
    engine->last_gpu_us = 0.0;
  }

  // Extract the results: hard decisions from the final LLR sign bits (fp16: the zero-copy host
  // buffer for LLS, the GPU fp16 working buffer for the layered/flooding paths, the Q16.16 pool
  // bit 31 for the async path).
  const decode_ctrl_t* ctrl = static_cast<const decode_ctrl_t*>(engine->buf_ctrl.contents);
  const uint16_t*      final_llr =
      is_lls ? static_cast<const uint16_t*>(mtl_llr_i8.contents)
             : static_cast<const uint16_t*>(engine->buf_llr_fp16.contents);
  if (out_bits != nullptr) {
    if (is_async) {
      const uint32_t* pool = static_cast<const uint32_t*>(engine->buf_p.contents);
      for (uint32_t i = 0; i != engine->n_info; ++i) {
        out_bits[i] = static_cast<uint8_t>((pool[i] >> 31) & 1U);
      }
    } else {
      for (uint32_t i = 0; i != engine->n_info; ++i) {
        out_bits[i] = static_cast<uint8_t>((final_llr[i] >> 15) & 1U);
      }
    }
  }
  if (error_count_out != nullptr) {
    // Recompute the final syndrome on the CPU: the kernels clear ctrl->error_count
    // at the end of every round, so it does not reflect the final state. LLS packs
    // one syndrome bit per check row (popcount over the packed words).
    uint32_t errors = 0;
    if (is_lls) {
      const uint32_t* h_pred = static_cast<const uint32_t*>(engine->buf_h_pred.contents);
      for (uint32_t w = 0; w != engine->h_pred_len; ++w) {
        errors += static_cast<uint32_t>(__builtin_popcount(h_pred[w]));
      }
    } else {
      const uint32_t* h_pred_bits = static_cast<const uint32_t*>(engine->buf_h_pred_bits.contents);
      for (uint32_t w = 0; w != engine->m_aligned; ++w) {
        errors += h_pred_bits[w] & 1u;
      }
    }
    *error_count_out = errors;
  }
  // The gate counts only rounds whose kernels actually ran; without ET the
  // layered mode keeps reporting max_iter for backward compatibility. LLS
  // always reports the rounds actually executed (its ET is GPU-internal).
  if (is_flooding || is_persistent || is_async || is_lls) {
    return static_cast<int>(ctrl->actual_iters);
  }
  return engine->et_enabled ? static_cast<int>(ctrl->actual_iters) : max_iter;
}

uint32_t decoder_engine::get_n_info() const
{
  const engine_impl_t* engine = static_cast<const engine_impl_t*>(impl);
  return engine != nullptr ? engine->n_info : 0;
}

double decoder_engine::last_gpu_wait_us() const
{
  const engine_impl_t* engine = static_cast<const engine_impl_t*>(impl);
  return engine != nullptr ? engine->last_gpu_us : 0.0;
}

} // namespace metal
} // namespace ocudu
