// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
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
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "ocudu_metal_decoder_engine.h"

namespace ocudu {
namespace metal {

// Must match the DecodeCtrl struct in ocudu_nms_layered_decoder.metal (shader ABI).
// The async kernel uses the same 3-word layout as its AsyncCtrl (stop_flag,
// row_updates, actual_gens - field names differ, offsets are identical).
struct decode_ctrl_t {
  uint32_t error_count;     // unsatisfied check equations (current round) / async: stop_flag
  uint32_t early_terminate; // set by the ET gate when the syndrome converges / async: row_updates
  uint32_t actual_iters;    // rounds actually executed / async: actual_gens
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

  // Zero-copy wrappers, cached by host pointer (the host buffers outlive the engine).
  std::unordered_map<const void*, id<MTLBuffer>> buffer_cache;

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
  // GPU-side duration of the last decode (0 when unavailable), for the
  // latency-benchmark breakdown: wall - gpu = CPU-side fixed overhead.
  double   last_gpu_us = 0.0;
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
    return it->second;
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
  engine->buffer_cache[ptr] = buf;
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
  std::lock_guard<std::mutex> lock(algo_resources_mutex());
  auto&                      cache = algo_resources_cache();
  auto                       it    = cache.find(static_cast<int>(mode));
  if (it != cache.end()) {
    return &it->second;
  }

  algo_resources_t res;
  res.device = MTLCreateSystemDefaultDevice();
  if (res.device == nil) {
    NSLog(@"ocudu Metal LDPC: no Metal device available");
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
  }
  NSString* lib_path = resolve_metallib_path(lib_path_macro, lib_name);
  if (lib_path == nil) {
    NSLog(@"ocudu Metal LDPC: pre-compiled shader library '%s' not found (searched the configure-time "
          @"path, next to the executable, and the working directory)",
          lib_name);
    return nullptr;
  }
  NSError*       error   = nil;
  id<MTLLibrary> library = [res.device newLibraryWithURL:[NSURL fileURLWithPath:lib_path] error:&error];
  if (library == nil) {
    NSLog(@"ocudu Metal LDPC: failed to load the pre-compiled shader library %@: %@", lib_path, error);
    return nullptr;
  }
  NSLog(@"ocudu Metal LDPC: loaded pre-compiled shader library %@", lib_path);

  auto make_pipeline = [&](id<MTLComputePipelineState> __strong* out, const char* name) -> bool {
    id<MTLFunction> fn = [library newFunctionWithName:[NSString stringWithUTF8String:name]];
    if (fn == nil) {
      NSLog(@"ocudu Metal LDPC: kernel '%s' not found in the shader library", name);
      return false;
    }
    *out = [res.device newComputePipelineStateWithFunction:fn error:&error];
    if (*out == nil) {
      NSLog(@"ocudu Metal LDPC: pipeline '%s' failed: %@", name, error);
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
    engine->buffer_cache.clear();
    delete engine;
    impl = nullptr;
  }
}

bool decoder_engine::init(uint32_t n_logical, uint32_t m_logical, float factor, float beta,
                          const uint32_t* h, const uint32_t* ht, const layered_info& layered,
                          algo mode, bool et_enabled)
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
  engine->buf_h = zero_copy_buffer(engine, h, static_cast<size_t>(engine->m_aligned) *
                                                 engine->n_h_chunks * sizeof(uint32_t));

  if (mode == algo::flooding) {
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

  return true;
}

int decoder_engine::decode(const void* in_fp16, uint8_t* out_bits, int max_iter, uint32_t* error_count_out)
{
  engine_impl_t* engine = static_cast<engine_impl_t*>(impl);
  const bool     is_persistent = engine != nullptr && engine->mode == decoder_engine::algo::layered_persistent;
  const bool     is_async      = engine != nullptr && engine->mode == decoder_engine::algo::async_delta;
  if (engine == nullptr ||
      (is_persistent ? engine->p_nmsl_persistent == nil
       : is_async    ? engine->p_nmsa_delta_bp == nil
                     : (engine->mode == decoder_engine::algo::flooding ? engine->p_nmsf_init == nil
                                                                       : engine->p_nmsl_init == nil))) {
    return -1;
  }
  const bool is_flooding = engine->mode == decoder_engine::algo::flooding;
  id<MTLBuffer> mtl_llr_i8 =
      zero_copy_buffer(engine, in_fp16, static_cast<size_t>(engine->n_aligned) * sizeof(int8_t));
  id<MTLCommandBuffer>        cmd_buf = [engine->queue commandBuffer];
  id<MTLComputeCommandEncoder> enc    = [cmd_buf computeCommandEncoder];

  if (is_flooding) {
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
  [enc setBytes:&engine->n_h_chunks length:sizeof(uint32_t) atIndex:14];
  [enc setBuffer:engine->buf_h offset:0 atIndex:15];
  [enc setBuffer:mtl_llr_i8 offset:0 atIndex:16];
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
  // Final syndrome: 10-14.
  [enc setBuffer:engine->buf_h offset:0 atIndex:10];
  [enc setBuffer:engine->buf_llr_fp16 offset:0 atIndex:11];
  [enc setBuffer:engine->buf_h_pred_bits offset:0 atIndex:12];
  [enc setBytes:&engine->n_h_chunks length:sizeof(uint32_t) atIndex:13];
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
  [cmd_buf waitUntilCompleted];
  if (cmd_buf.status != MTLCommandBufferStatusCompleted) {
    NSLog(@"ocudu Metal LDPC: command buffer failed with status %lu",
          static_cast<unsigned long>(cmd_buf.status));
    return -1;
  }
  // GPU-side duration (Apple Silicon: valid for compute command buffers; 0 when unavailable).
  if (cmd_buf.GPUStartTime > 0.0 && cmd_buf.GPUEndTime > 0.0) {
    engine->last_gpu_us = (cmd_buf.GPUEndTime - cmd_buf.GPUStartTime) * 1e6;
  } else {
    engine->last_gpu_us = 0.0;
  }

  // Extract the results: hard decisions from the final LLR sign bits (fp16
  // for the layered/flooding paths, the Q16.16 pool bit 31 for the async
  // path).
  const decode_ctrl_t* ctrl      = static_cast<const decode_ctrl_t*>(engine->buf_ctrl.contents);
  const uint16_t*      final_llr = static_cast<const uint16_t*>(engine->buf_llr_fp16.contents);
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
    // Recompute the final syndrome on the CPU: the gate clears ctrl->error_count
    // at the end of every round, so it does not reflect the final state.
    const uint32_t* h_pred_bits = static_cast<const uint32_t*>(engine->buf_h_pred_bits.contents);
    uint32_t        errors      = 0;
    for (uint32_t w = 0; w != engine->m_aligned; ++w) {
      errors += h_pred_bits[w] & 1u;
    }
    *error_count_out = errors;
  }
  // The gate counts only rounds whose kernels actually ran; without ET the
  // layered mode keeps reporting max_iter for backward compatibility.
  if (is_flooding || is_persistent || is_async) {
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
