// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu_equalizer_metal_engine.h"
#include "ocudu_metal_burst.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "ocudu_metal_queue.h"

#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/support/macos_compat.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

#ifndef OCUDU_EQUALIZER_METALLIB_PATH
#define OCUDU_EQUALIZER_METALLIB_PATH "ocudu_equalizer.metallib"
#endif

using namespace ocudu;

namespace ocudu {
namespace metal {

namespace {

// ---- Process-wide dispatch/wait statistics (same accounting as the other engines) ----
#if defined(OCUDU_METAL_STATS)
struct eq_stats_t {
  std::atomic<uint64_t> commits{0};
  std::atomic<uint64_t> waits{0};
  std::atomic<uint64_t> in_flight{0};
  std::atomic<uint64_t> in_flight_max{0};
};
static eq_stats_t& eq_stats()
{
  static eq_stats_t s;
  return s;
}
static void eq_stats_commit()
{
  eq_stats_t& s = eq_stats();
  s.commits.fetch_add(1, std::memory_order_relaxed);
  const uint64_t nf = s.in_flight.fetch_add(1, std::memory_order_acq_rel) + 1;
  uint64_t       prev = s.in_flight_max.load(std::memory_order_relaxed);
  while (nf > prev && !s.in_flight_max.compare_exchange_weak(prev, nf, std::memory_order_relaxed)) {
  }
}
static void eq_stats_wait()
{
  eq_stats_t& s = eq_stats();
  s.waits.fetch_add(1, std::memory_order_relaxed);
  s.in_flight.fetch_sub(1, std::memory_order_acq_rel);
}
static void eq_stats_report()
{
  const eq_stats_t& s = eq_stats();
  std::fprintf(stderr,
               "[metal_stats] equalizer commits=%llu waits=%llu max_in_flight=%llu (synchronous "
               "path only; deferred group dispatches are counted by [metal_stats] burst)\n",
               static_cast<unsigned long long>(s.commits.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.waits.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.in_flight_max.load(std::memory_order_relaxed)));
}
#else
static void eq_stats_commit() {}
static void eq_stats_wait() {}
#endif // OCUDU_METAL_STATS

struct eq_resources_t {
  id<MTLDevice>               device         = nil;
  id<MTLCommandQueue>         queue          = nil;
  id<MTLComputePipelineState> pipeline       = nil;
  id<MTLComputePipelineState> pipeline_batch = nil;
};

/// Per-symbol element strides handed to equalize_mxn_batch(); must match struct equalize_strides in
/// the shader source.
struct eq_strides_t {
  unsigned nof_symbols;
  unsigned h_stride;
  unsigned y_stride;
  unsigned eq_stride;
  unsigned nv_stride;
};
static eq_resources_t& eq_resources()
{
  static eq_resources_t r;
  return r;
}
static std::mutex& eq_resources_mutex()
{
  static std::mutex m;
  return m;
}

NSString* resolve_eq_metallib_path()
{
  NSMutableArray<NSString*>* candidates = [NSMutableArray arrayWithCapacity:3];
  [candidates addObject:[NSString stringWithUTF8String:OCUDU_EQUALIZER_METALLIB_PATH]];
  NSArray<NSString*>* args = [[NSProcessInfo processInfo] arguments];
  if (args.count > 0) {
    [candidates addObject:[[args[0] stringByDeletingLastPathComponent]
                              stringByAppendingPathComponent:@"ocudu_equalizer.metallib"]];
  }
  [candidates addObject:[[[NSFileManager defaultManager] currentDirectoryPath]
                            stringByAppendingPathComponent:@"ocudu_equalizer.metallib"]];
  NSFileManager* fm = [NSFileManager defaultManager];
  for (NSString* path in candidates) {
    if ([fm fileExistsAtPath:path]) {
      return path;
    }
  }
  return nil;
}

// Must match equalize_params in ocudu_equalizer.metal.
struct equalize_params_t {
  uint32_t nof_re;
  uint32_t nof_ports;
  uint32_t nof_layers;
  uint32_t algo;
  float    noise_var;
  float    tx_scaling;
  float    h_scaling;
};

struct eq_engine_impl {
  double last_gpu_us = 0.0;
  bool   last_call_no_copy = true; // false when any buffer of the last call was copied
  /// Batched group dispatches encoded so far (diagnostics: proves that a group really took the
  /// batched kernel instead of falling back to one dispatch per symbol).
  unsigned batch_dispatches = 0;
  bool   no_copy_fallback_logged = false;
  /// Command buffers committed and not waited for yet. Metal only serializes the *start* of the
  /// command buffers of one queue and lets them overlap, so a wait has to cover every one of them
  /// and not only the newest.
  std::vector<id<MTLCommandBuffer>> outstanding;
  std::unordered_map<const void*, std::pair<id<MTLBuffer>, size_t>> buffer_cache;

  // Batch in progress (nil when no batch is open).
  id<MTLCommandBuffer>         batch_cb  = nil;
  id<MTLComputeCommandEncoder> batch_enc = nil;
  unsigned                     batch_n   = 0;
};

id<MTLBuffer> wrap_buffer(eq_engine_impl* engine, const void* ptr, size_t length)
{
  // One buffer object per address for every engine: the stages of the chain write and read the same
  // memory, and Metal only relates accesses through the resource they are bound to (see
  // shared_queue::wrap_no_copy).
  id<MTLBuffer> buf = metal::shared_queue::wrap_no_copy(metal::shared_queue::device(), ptr, length);
  if (buf != nil) {
    return buf;
  }
  engine->last_call_no_copy = false;
  if (!engine->no_copy_fallback_logged) {
    engine->no_copy_fallback_logged = true;
    ocudulog::fetch_basic_logger("PHY").warning(
        "Metal equalizer: no-copy buffer wrap failed (ptr {} length {} page {}); falling back to a staging copy",
        ptr,
        length,
        compat::page_size());
  }
  return [metal::shared_queue::device() newBufferWithBytes:ptr length:length options:MTLResourceStorageModeShared];
}

} // namespace

equalizer_metal_engine::~equalizer_metal_engine()
{
  eq_engine_impl* engine = static_cast<eq_engine_impl*>(impl);
  delete engine;
  impl = nullptr;
}

bool equalizer_metal_engine::init()
{
#if defined(OCUDU_METAL_STATS)
  static std::once_flag stats_atexit_flag;
  std::call_once(stats_atexit_flag, []() { std::atexit(eq_stats_report); });
#endif

  if (impl == nullptr) {
    impl = new eq_engine_impl();
  }

  std::lock_guard<std::mutex> lock(eq_resources_mutex());
  eq_resources_t& res = eq_resources();
  if (res.device == nil) {
    res.device = MTLCreateSystemDefaultDevice();
    if (res.device == nil) {
      ocudulog::fetch_basic_logger("PHY").error("Metal equalizer: no Metal device available");
      return false;
    }
    res.queue = metal::shared_queue::backend_queue();
    NSString* lib_path = resolve_eq_metallib_path();
    if (lib_path == nil) {
      ocudulog::fetch_basic_logger("PHY").error(
          "Metal equalizer: pre-compiled shader library 'ocudu_equalizer.metallib' not found");
      return false;
    }
    NSError*       error   = nil;
    id<MTLLibrary> library = [res.device newLibraryWithURL:[NSURL fileURLWithPath:lib_path] error:&error];
    if (library == nil) {
      ocudulog::fetch_basic_logger("PHY").error("Metal equalizer: failed to load the shader library {}: {}",
                                                lib_path.UTF8String,
                                                error != nil ? error.localizedDescription.UTF8String : "nil error");
      return false;
    }
    id<MTLFunction> fn = [library newFunctionWithName:@"equalize_mxn"];
    if (fn == nil) {
      ocudulog::fetch_basic_logger("PHY").error("Metal equalizer: kernel 'equalize_mxn' not found");
      return false;
    }
    res.pipeline = [res.device newComputePipelineStateWithFunction:fn error:&error];
    // One dispatch for a whole group of symbols, same arithmetic (see equalize_mxn_batch).
    id<MTLFunction> fn_batch = [library newFunctionWithName:@"equalize_mxn_batch"];
    if (fn_batch == nil) {
      ocudulog::fetch_basic_logger("PHY").error("Metal equalizer: kernel 'equalize_mxn_batch' not found");
      return false;
    }
    res.pipeline_batch = [res.device newComputePipelineStateWithFunction:fn_batch error:&error];
    if (res.pipeline == nil) {
      ocudulog::fetch_basic_logger("PHY").error("Metal equalizer: pipeline creation failed: {}",
                                                error != nil ? error.localizedDescription.UTF8String : "nil error");
      return false;
    }
    ocudulog::fetch_basic_logger("PHY").debug("Metal equalizer: loaded pre-compiled shader library {}",
                                              lib_path.UTF8String);
  }
  return true;
}

bool equalizer_metal_engine::begin_batch()
{
  eq_engine_impl* engine = static_cast<eq_engine_impl*>(impl);
  if (engine == nullptr || eq_resources().pipeline == nil) {
    return false;
  }
  if (engine->batch_cb != nil) {
    return false; // a batch is already open
  }
  engine->batch_cb           = [eq_resources().queue commandBuffer];
  engine->batch_enc          = [engine->batch_cb computeCommandEncoder];
  engine->batch_n            = 0;
  [engine->batch_enc setComputePipelineState:eq_resources().pipeline];
  return engine->batch_enc != nil;
}

bool equalizer_metal_engine::enqueue(const void* h,
                                     const void* y,
                                     const void* sigma2,
                                     void*       eq,
                                     void*       nv,
                                     unsigned    nof_re,
                                     unsigned    nof_ports,
                                     unsigned    nof_layers,
                                     bool        mmse,
                                     float       noise_var,
                                     float       tx_scaling,
                                     float       h_scaling)
{
  eq_engine_impl* engine = static_cast<eq_engine_impl*>(impl);
  if (engine == nullptr || engine->batch_enc == nil) {
    return false;
  }
  // cbf16_t inputs: 4 bytes per complex sample, widened inside the kernel.
  const size_t h_bytes = static_cast<size_t>(nof_ports) * nof_layers * nof_re * 4;
  const size_t y_bytes = static_cast<size_t>(nof_ports) * nof_re * 4;
  const size_t s_bytes = static_cast<size_t>(nof_ports) * sizeof(float);
  const size_t eq_bytes = static_cast<size_t>(nof_layers) * nof_re * 2 * sizeof(float);
  const size_t nv_bytes = static_cast<size_t>(nof_layers) * nof_re * sizeof(float);
  // wrap_buffer() clears this flag when a no-copy wrap falls back to a copy. Reset it before the
  // wraps (not after, where it would overwrite the outcome) so the diagnostic reports the truth.
  engine->last_call_no_copy = true;
  id<MTLBuffer> b_h  = wrap_buffer(engine, h, h_bytes);
  id<MTLBuffer> b_y  = wrap_buffer(engine, y, y_bytes);
  id<MTLBuffer> b_s  = wrap_buffer(engine, sigma2, s_bytes);
  id<MTLBuffer> b_eq = wrap_buffer(engine, eq, eq_bytes);
  id<MTLBuffer> b_nv = wrap_buffer(engine, nv, nv_bytes);
  if (b_h == nil || b_y == nil || b_s == nil || b_eq == nil || b_nv == nil) {
    return false;
  }
  equalize_params_t params{nof_re, nof_ports, nof_layers, mmse ? 1u : 0u, noise_var, tx_scaling, h_scaling};
  id<MTLComputeCommandEncoder> enc = engine->batch_enc;
  [enc setBuffer:b_h offset:0 atIndex:0];
  [enc setBuffer:b_y offset:0 atIndex:1];
  [enc setBuffer:b_eq offset:0 atIndex:2];
  [enc setBuffer:b_nv offset:0 atIndex:3];
  [enc setBuffer:b_s offset:0 atIndex:5];
  [enc setBytes:&params length:sizeof(params) atIndex:4];
  [enc dispatchThreads:MTLSizeMake(nof_re, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
  ++engine->batch_n;
  return true;
}

bool equalizer_metal_engine::batch_open() const
{
  const eq_engine_impl* engine = static_cast<const eq_engine_impl*>(impl);
  return (engine != nullptr) && (engine->batch_cb != nil);
}

bool equalizer_metal_engine::enqueue_burst(const void* h,
                                          const void* y,
                                          const void* sigma2,
                                          void*       eq,
                                          void*       nv,
                                          unsigned    nof_re,
                                          unsigned    nof_ports,
                                          unsigned    nof_layers,
                                          bool        mmse,
                                          float       noise_var,
                                          float       tx_scaling,
                                          float       h_scaling)
{
  eq_engine_impl* engine = static_cast<eq_engine_impl*>(impl);
  if (engine == nullptr) {
    return false;
  }
  id<MTLComputeCommandEncoder> enc = metal::shared_burst::encoder(eq_resources().pipeline);
  if (enc == nil) {
    return false;
  }
  const size_t h_bytes  = static_cast<size_t>(nof_ports) * nof_layers * nof_re * sizeof(cbf16_t);
  const size_t y_bytes  = static_cast<size_t>(nof_ports) * nof_re * sizeof(cbf16_t);
  const size_t s_bytes  = static_cast<size_t>(nof_ports) * sizeof(float);
  const size_t eq_bytes = static_cast<size_t>(nof_layers) * nof_re * 2 * sizeof(float);
  const size_t nv_bytes = static_cast<size_t>(nof_layers) * nof_re * sizeof(float);

  id<MTLBuffer> b_h  = wrap_buffer(engine, h, h_bytes);
  id<MTLBuffer> b_y  = wrap_buffer(engine, y, y_bytes);
  id<MTLBuffer> b_s  = wrap_buffer(engine, sigma2, s_bytes);
  id<MTLBuffer> b_eq = wrap_buffer(engine, eq, eq_bytes);
  id<MTLBuffer> b_nv = wrap_buffer(engine, nv, nv_bytes);
  if (b_h == nil || b_y == nil || b_s == nil || b_eq == nil || b_nv == nil) {
    engine->last_call_no_copy = false;
    return false;
  }
  engine->last_call_no_copy         = true;
  const equalize_params_t params{nof_re, nof_ports, nof_layers, mmse ? 1u : 0u, noise_var, tx_scaling, h_scaling};
  [enc setBuffer:b_h offset:0 atIndex:0];
  [enc setBuffer:b_y offset:0 atIndex:1];
  [enc setBuffer:b_eq offset:0 atIndex:2];
  [enc setBuffer:b_nv offset:0 atIndex:3];
  [enc setBytes:&params length:sizeof(params) atIndex:4];
  [enc setBuffer:b_s offset:0 atIndex:5];
  [enc dispatchThreads:MTLSizeMake(nof_re, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
  metal::shared_burst::count_dispatch(metal::shared_burst::stage::equalizer);
  return true;
}

bool equalizer_metal_engine::enqueue_burst_batch(const void* h,
                                                const void* y,
                                                const void* sigma2,
                                                void*       eq,
                                                void*       nv,
                                                unsigned    nof_re,
                                                unsigned    nof_symbols,
                                                unsigned    h_symbol_stride,
                                                unsigned    y_symbol_stride,
                                                unsigned    eq_symbol_stride,
                                                unsigned    nv_symbol_stride,
                                                unsigned    nof_ports,
                                                unsigned    nof_layers,
                                                bool        mmse,
                                                float       noise_var,
                                                float       tx_scaling,
                                                float       h_scaling)
{
  eq_engine_impl* engine = static_cast<eq_engine_impl*>(impl);
  if ((engine == nullptr) || (nof_symbols == 0)) {
    return false;
  }
  id<MTLComputeCommandEncoder> enc = metal::shared_burst::encoder(eq_resources().pipeline_batch);
  if (enc == nil) {
    return false;
  }
  // The bound buffers cover the whole group: one symbol's worth plus the stride to the next.
  const size_t h_bytes  = static_cast<size_t>(nof_ports) * nof_layers *
                          ((static_cast<size_t>(nof_symbols) - 1) * h_symbol_stride + nof_re) * sizeof(cbf16_t);
  const size_t y_bytes  = static_cast<size_t>(nof_ports) *
                          ((static_cast<size_t>(nof_symbols) - 1) * y_symbol_stride + nof_re) * sizeof(cbf16_t);
  const size_t s_bytes  = static_cast<size_t>(nof_ports) * sizeof(float);
  const size_t eq_bytes = ((static_cast<size_t>(nof_symbols) - 1) * eq_symbol_stride + nof_re * nof_layers) * 2 *
                          sizeof(float);
  const size_t nv_bytes = ((static_cast<size_t>(nof_symbols) - 1) * nv_symbol_stride + nof_re * nof_layers) *
                          sizeof(float);

  id<MTLBuffer> b_h  = wrap_buffer(engine, h, h_bytes);
  id<MTLBuffer> b_y  = wrap_buffer(engine, y, y_bytes);
  id<MTLBuffer> b_s  = wrap_buffer(engine, sigma2, s_bytes);
  id<MTLBuffer> b_eq = wrap_buffer(engine, eq, eq_bytes);
  id<MTLBuffer> b_nv = wrap_buffer(engine, nv, nv_bytes);
  if (b_h == nil || b_y == nil || b_s == nil || b_eq == nil || b_nv == nil) {
    engine->last_call_no_copy = false;
    return false;
  }
  engine->last_call_no_copy          = true;
  const equalize_params_t params{nof_re, nof_ports, nof_layers, mmse ? 1u : 0u, noise_var, tx_scaling, h_scaling};
  const eq_strides_t      strides{nof_symbols, h_symbol_stride, y_symbol_stride, eq_symbol_stride, nv_symbol_stride};
  [enc setBuffer:b_h offset:0 atIndex:0];
  [enc setBuffer:b_y offset:0 atIndex:1];
  [enc setBuffer:b_eq offset:0 atIndex:2];
  [enc setBuffer:b_nv offset:0 atIndex:3];
  [enc setBytes:&params length:sizeof(params) atIndex:4];
  [enc setBuffer:b_s offset:0 atIndex:5];
  [enc setBytes:&strides length:sizeof(strides) atIndex:6];
  [enc dispatchThreads:MTLSizeMake(nof_re, nof_symbols, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
  metal::shared_burst::count_dispatch(metal::shared_burst::stage::equalizer);
  ++engine->batch_dispatches;
  return true;
}

bool equalizer_metal_engine::burst_open()
{
  return metal::shared_burst::open();
}

bool equalizer_metal_engine::burst_commit()
{
  return metal::shared_burst::commit();
}

bool equalizer_metal_engine::burst_wait_committed()
{
  return metal::shared_burst::wait_committed();
}

bool equalizer_metal_engine::commit_batch()
{
  eq_engine_impl* engine = static_cast<eq_engine_impl*>(impl);
  if (engine == nullptr || engine->batch_cb == nil) {
    return false;
  }
  id<MTLCommandBuffer>         cmd_buf = engine->batch_cb;
  id<MTLComputeCommandEncoder> enc     = engine->batch_enc;
  engine->batch_cb  = nil;
  engine->batch_enc = nil;
  engine->batch_n   = 0;

  [enc endEncoding];
  [cmd_buf commit];
  eq_stats_commit();
  engine->outstanding.push_back(cmd_buf);
  return true;
}

bool equalizer_metal_engine::wait_committed()
{
  eq_engine_impl* engine = static_cast<eq_engine_impl*>(impl);
  if (engine == nullptr || engine->outstanding.empty()) {
    return true;
  }
  std::vector<id<MTLCommandBuffer>> outstanding;
  outstanding.swap(engine->outstanding);
  bool ok = true;
  for (id<MTLCommandBuffer> cmd_buf : outstanding) {
    [cmd_buf waitUntilCompleted];
    eq_stats_wait();
    if (cmd_buf.status != MTLCommandBufferStatusCompleted) {
      ocudulog::fetch_basic_logger("PHY").error("Metal equalizer: command buffer failed with status {}",
                                                static_cast<unsigned long>(cmd_buf.status));
      ok = false;
    }
    if (cmd_buf.GPUStartTime > 0.0 && cmd_buf.GPUEndTime > 0.0) {
      engine->last_gpu_us = (cmd_buf.GPUEndTime - cmd_buf.GPUStartTime) * 1e6;
    }
  }
  return ok;
}

bool equalizer_metal_engine::flush_batch()
{
  eq_engine_impl* engine = static_cast<eq_engine_impl*>(impl);
  if (engine == nullptr || engine->batch_cb == nil) {
    return false;
  }
  id<MTLCommandBuffer>         cmd_buf = engine->batch_cb;
  id<MTLComputeCommandEncoder> enc     = engine->batch_enc;
  engine->batch_cb  = nil;
  engine->batch_enc = nil;
  engine->batch_n   = 0;

  [enc endEncoding];
  [cmd_buf commit];
  eq_stats_commit();
  // Register the command buffer and drain every outstanding one through the same path the deferred
  // entry point uses, so both the accounting and the wait cover the whole chain.
  engine->outstanding.push_back(cmd_buf);
  const bool ok = wait_committed();
  if (cmd_buf.status == MTLCommandBufferStatusCompleted) {
    if (cmd_buf.GPUStartTime > 0.0 && cmd_buf.GPUEndTime > 0.0) {
      engine->last_gpu_us = (cmd_buf.GPUEndTime - cmd_buf.GPUStartTime) * 1e6;
    } else {
      engine->last_gpu_us = 0.0;
    }
  }
  return ok;
}

unsigned equalizer_metal_engine::batch_size() const
{
  const eq_engine_impl* engine = static_cast<const eq_engine_impl*>(impl);
  return engine != nullptr ? engine->batch_n : 0;
}

bool equalizer_metal_engine::equalize(const void* h,
                                      const void* y,
                                      const void* sigma2,
                                      void*       eq,
                                      void*       nv,
                                      unsigned    nof_re,
                                      unsigned    nof_ports,
                                      unsigned    nof_layers,
                                      bool        mmse,
                                      float       noise_var,
                                      float       tx_scaling,
                                      float       h_scaling)
{
  // Compatibility wrapper: one dispatch per command buffer.
  if (!begin_batch()) {
    return false;
  }
  if (!enqueue(h, y, sigma2, eq, nv, nof_re, nof_ports, nof_layers, mmse, noise_var, tx_scaling, h_scaling)) {
    flush_batch();
    return false;
  }
  return flush_batch();
}

bool equalizer_metal_engine::last_call_used_no_copy() const
{
  const eq_engine_impl* engine = static_cast<const eq_engine_impl*>(impl);
  return engine != nullptr ? engine->last_call_no_copy : false;
}

unsigned equalizer_metal_engine::batch_dispatch_count() const
{
  auto* engine = static_cast<eq_engine_impl*>(impl);
  return (engine == nullptr) ? 0 : engine->batch_dispatches;
}

double equalizer_metal_engine::last_gpu_wait_us() const
{
  const eq_engine_impl* engine = static_cast<const eq_engine_impl*>(impl);
  return engine != nullptr ? engine->last_gpu_us : 0.0;
}

} // namespace metal
} // namespace ocudu
