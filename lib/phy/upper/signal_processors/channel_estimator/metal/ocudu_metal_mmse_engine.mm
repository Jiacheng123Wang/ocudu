// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu_metal_mmse_engine.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "ocudu/ocudulog/ocudulog.h"

#include <cstring>
#include <mutex>
#include <unordered_map>

#if !defined(OCUDU_MMSE_METALLIB_PATH)
#define OCUDU_MMSE_METALLIB_PATH "ocudu_mmse.metallib"
#endif

using namespace ocudu;

namespace {

struct mmse_engine_impl {
  id<MTLDevice>                  device      = nil;
  id<MTLCommandQueue>            queue       = nil;
  id<MTLLibrary>                 library     = nil;
  id<MTLComputePipelineState>    inv_pipe    = nil;
  id<MTLComputePipelineState>    weights_pipe = nil;
  id<MTLComputePipelineState>    apply_pipe  = nil;
  // metal_nn_mmse: simdgroup_matrix 8x8 pipelines (optional, loaded on demand).
  id<MTLComputePipelineState>    weights_matrix_pipe = nil;
  id<MTLComputePipelineState>    apply_matrix_pipe  = nil;
  std::unordered_map<const void*, id<MTLBuffer>> buffer_cache;
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
        return it->second;
      }
    }
    id<MTLBuffer> buf = [device newBufferWithBytesNoCopy:const_cast<void*>(ptr)
                                                 length:bytes
                                                options:MTLResourceStorageModeShared
                                            deallocator:nil];
    if (buf != nil) {
      std::lock_guard<std::mutex> lock(cache_mutex);
      buffer_cache.emplace(ptr, buf);
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

} // namespace

namespace ocudu {
namespace metal {

mmse_engine::~mmse_engine()
{
  delete static_cast<mmse_engine_impl*>(impl);
}

bool mmse_engine::init(const char* metallib_path)
{
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
  e->queue = [e->device newCommandQueue];
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
  // ARC-managed; no explicit release.
  return e->inv_pipe != nil && e->weights_pipe != nil && e->apply_pipe != nil;
}

bool mmse_engine::invert(float* a, unsigned n, unsigned nof_systems)
{
  auto* e = static_cast<mmse_engine_impl*>(impl);
  if (e == nullptr || e->device == nil) {
    return false;
  }
  if (n > 36) {
    // The K1 kernel uses fixed-size threadgroup memory for 36x36; larger systems must use
    // the CPU inversion (the hot path does that anyway).
    return false;
  }

  const NSUInteger bytes = static_cast<NSUInteger>(nof_systems) * n * n * sizeof(float);
  id<MTLBuffer>    a_buf = e->wrap(a, bytes);
  if (a_buf == nil) {
    return false;
  }

  id<MTLCommandBuffer> cb = [e->queue commandBuffer];
  id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
  [enc setComputePipelineState:e->inv_pipe];
  [enc setBuffer:a_buf offset:0 atIndex:0];
  [enc setBytes:&n length:sizeof(unsigned) atIndex:1];
  [enc setBytes:&nof_systems length:sizeof(unsigned) atIndex:2];
  [enc dispatchThreadgroups:MTLSizeMake(nof_systems, 1, 1)
      threadsPerThreadgroup:MTLSizeMake(n, 1, 1)];
  [enc endEncoding];
  [cb commit];
  [cb waitUntilCompleted];

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
  [cb waitUntilCompleted];

  if (cb.status != MTLCommandBufferStatusCompleted || cb.error != nil) {
    return false;
  }
  if (cb.GPUStartTime != 0 && cb.GPUEndTime != 0) {
    e->last_gpu_us = (cb.GPUEndTime - cb.GPUStartTime) * 1e6;
  }
  return true;
}

bool mmse_engine::run(float* a, const float* r_hp, float* w, const float* y, float* h, unsigned nout,
                      unsigned L, unsigned nof_systems, unsigned nof_blocks)
{
  auto* e = static_cast<mmse_engine_impl*>(impl);
  if (e == nullptr || e->device == nil) {
    return false;
  }

  id<MTLBuffer> a_buf  = e->wrap(a, static_cast<NSUInteger>(nof_systems) * L * L * sizeof(float));
  id<MTLBuffer> rp_buf = e->wrap(r_hp, static_cast<NSUInteger>(nof_systems) * nout * L * sizeof(float));
  id<MTLBuffer> w_buf  = e->wrap(w, static_cast<NSUInteger>(nof_systems) * nout * L * sizeof(float));
  id<MTLBuffer> y_buf  = e->wrap(y, static_cast<NSUInteger>(nof_systems) * nof_blocks * 2 * L * sizeof(float));
  id<MTLBuffer> h_buf  = e->wrap(h, static_cast<NSUInteger>(nof_systems) * nof_blocks * 2 * nout * sizeof(float));
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
  id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];

  [enc setComputePipelineState:e->inv_pipe];
  [enc setBuffer:a_buf offset:0 atIndex:0];
  [enc setBytes:&L length:sizeof(unsigned) atIndex:1];
  [enc setBytes:&nof_systems length:sizeof(unsigned) atIndex:2];
  [enc dispatchThreadgroups:MTLSizeMake(nof_systems, 1, 1) threadsPerThreadgroup:MTLSizeMake(L, 1, 1)];

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

  [enc endEncoding];
  [cb commit];
  [cb waitUntilCompleted];

  if (cb.status != MTLCommandBufferStatusCompleted || cb.error != nil) {
    return false;
  }
  if (cb.GPUStartTime != 0 && cb.GPUEndTime != 0) {
    e->last_gpu_us = (cb.GPUEndTime - cb.GPUStartTime) * 1e6;
  }
  return true;
}

bool mmse_engine::run_weights_only(const float* a_inv, const float* r_hp, float* w, const float* y, float* h,
                                 unsigned nout, unsigned L, unsigned nof_systems, unsigned nof_blocks)
{
  auto* e = static_cast<mmse_engine_impl*>(impl);
  if (e == nullptr || e->device == nil) {
    return false;
  }

  id<MTLBuffer> ai_buf = e->wrap(a_inv, static_cast<NSUInteger>(nof_systems) * L * L * sizeof(float));
  id<MTLBuffer> rp_buf = e->wrap(r_hp, static_cast<NSUInteger>(nof_systems) * nout * L * sizeof(float));
  id<MTLBuffer> w_buf  = e->wrap(w, static_cast<NSUInteger>(nof_systems) * nout * L * sizeof(float));
  id<MTLBuffer> y_buf  = e->wrap(y, static_cast<NSUInteger>(nof_systems) * nof_blocks * 2 * L * sizeof(float));
  id<MTLBuffer> h_buf  = e->wrap(h, static_cast<NSUInteger>(nof_systems) * nof_blocks * 2 * nout * sizeof(float));
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

  [enc endEncoding];
  [cb commit];
  [cb waitUntilCompleted];

  if (cb.status != MTLCommandBufferStatusCompleted || cb.error != nil) {
    return false;
  }
  if (cb.GPUStartTime != 0 && cb.GPUEndTime != 0) {
    e->last_gpu_us = (cb.GPUEndTime - cb.GPUStartTime) * 1e6;
  }
  return true;
}

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
  [cb waitUntilCompleted];

  if (cb.status != MTLCommandBufferStatusCompleted || cb.error != nil) {
    return false;
  }
  if (cb.GPUStartTime != 0 && cb.GPUEndTime != 0) {
    e->last_gpu_us = (cb.GPUEndTime - cb.GPUStartTime) * 1e6;
  }
  return true;
}

double mmse_engine::last_gpu_wait_us() const
{
  const auto* e = static_cast<const mmse_engine_impl*>(impl);
  return e == nullptr ? 0.0 : e->last_gpu_us;
}

} // namespace metal
} // namespace ocudu
