// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu_demod_metal_engine.h"
#include "ocudu_metal_burst.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "ocudu_metal_queue.h"

#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/support/macos_compat.h"

#include <atomic>
#include <cstdio>
#include <vector>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

#ifndef OCUDU_DEMOD_METALLIB_PATH
#define OCUDU_DEMOD_METALLIB_PATH "ocudu_demod.metallib"
#endif

using namespace ocudu;

namespace ocudu {
namespace metal {

namespace {

// ---- Process-wide dispatch/wait statistics (same accounting as the other engines) ----
#if defined(OCUDU_METAL_STATS)
struct demod_stats_t {
  std::atomic<uint64_t> commits{0};
  std::atomic<uint64_t> waits{0};
  std::atomic<uint64_t> in_flight{0};
  std::atomic<uint64_t> in_flight_max{0};
};
static demod_stats_t& demod_stats()
{
  static demod_stats_t s;
  return s;
}
static void demod_stats_commit()
{
  demod_stats_t& s = demod_stats();
  s.commits.fetch_add(1, std::memory_order_relaxed);
  const uint64_t nf = s.in_flight.fetch_add(1, std::memory_order_acq_rel) + 1;
  uint64_t       prev = s.in_flight_max.load(std::memory_order_relaxed);
  while (nf > prev && !s.in_flight_max.compare_exchange_weak(prev, nf, std::memory_order_relaxed)) {
  }
}
static void demod_stats_wait()
{
  demod_stats_t& s = demod_stats();
  s.waits.fetch_add(1, std::memory_order_relaxed);
  s.in_flight.fetch_sub(1, std::memory_order_acq_rel);
}
static void demod_stats_report()
{
  const demod_stats_t& s = demod_stats();
  std::fprintf(stderr,
               "[metal_stats] demapper commits=%llu waits=%llu max_in_flight=%llu (synchronous "
               "path only; deferred group dispatches are counted by [metal_stats] burst)\n",
               static_cast<unsigned long long>(s.commits.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.waits.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.in_flight_max.load(std::memory_order_relaxed)));
}
#else
static void demod_stats_commit() {}
static void demod_stats_wait() {}
#endif // OCUDU_METAL_STATS

struct demod_resources_t {
  id<MTLDevice>               device   = nil;
  id<MTLCommandQueue>         queue    = nil;
  id<MTLComputePipelineState> pipeline = nil;
};
static demod_resources_t& demod_resources()
{
  static demod_resources_t r;
  return r;
}
static std::mutex& demod_resources_mutex()
{
  static std::mutex m;
  return m;
}

NSString* resolve_demod_metallib_path()
{
  NSMutableArray<NSString*>* candidates = [NSMutableArray arrayWithCapacity:3];
  [candidates addObject:[NSString stringWithUTF8String:OCUDU_DEMOD_METALLIB_PATH]];
  NSArray<NSString*>* args = [[NSProcessInfo processInfo] arguments];
  if (args.count > 0) {
    [candidates addObject:[[args[0] stringByDeletingLastPathComponent]
                              stringByAppendingPathComponent:@"ocudu_demod.metallib"]];
  }
  [candidates addObject:[[[NSFileManager defaultManager] currentDirectoryPath]
                            stringByAppendingPathComponent:@"ocudu_demod.metallib"]];
  NSFileManager* fm = [NSFileManager defaultManager];
  for (NSString* path in candidates) {
    if ([fm fileExistsAtPath:path]) {
      return path;
    }
  }
  return nil;
}

// Must match demod_params in ocudu_demod.metal.
struct demod_params_t {
  uint32_t nof_symbols;
  uint32_t mod;
};

struct demod_engine_impl {
  double last_gpu_us = 0.0;
  // Batch in progress (nil when no batch is open).
  id<MTLCommandBuffer>         batch_cb  = nil;
  id<MTLComputeCommandEncoder> batch_enc = nil;
  unsigned                     batch_n   = 0;
  /// Command buffers committed and not waited for yet. Metal only serializes the *start* of the
  /// command buffers of one queue and lets them overlap, so a wait has to cover every one of them
  /// and not only the newest.
  std::vector<id<MTLCommandBuffer>> outstanding;
  bool   last_call_no_copy = true; // false when any buffer of the last call was copied
  bool   no_copy_fallback_logged = false;
  std::unordered_map<const void*, std::pair<id<MTLBuffer>, size_t>> buffer_cache;
};

id<MTLBuffer> wrap_buffer(demod_engine_impl* engine, const void* ptr, size_t length)
{
  // Same shared cache as the other engines: the demapper reads the symbols that the equalizer wrote
  // through the very same buffer object, so Metal tracks the dependency (see
  // shared_queue::wrap_no_copy).
  id<MTLBuffer> buf = metal::shared_queue::wrap_no_copy(metal::shared_queue::device(), ptr, length);
  if (buf != nil) {
    return buf;
  }
  engine->last_call_no_copy = false;
  if (!engine->no_copy_fallback_logged) {
    engine->no_copy_fallback_logged = true;
    ocudulog::fetch_basic_logger("PHY").warning(
        "Metal demapper: no-copy buffer wrap failed (ptr {} length {} page {}); falling back to a staging copy",
        ptr,
        length,
        compat::page_size());
  }
  return [metal::shared_queue::device() newBufferWithBytes:ptr length:length options:MTLResourceStorageModeShared];
}

} // namespace

demod_metal_engine::~demod_metal_engine()
{
  demod_engine_impl* engine = static_cast<demod_engine_impl*>(impl);
  delete engine;
  impl = nullptr;
}

bool demod_metal_engine::init()
{
#if defined(OCUDU_METAL_STATS)
  static std::once_flag stats_atexit_flag;
  std::call_once(stats_atexit_flag, []() { std::atexit(demod_stats_report); });
#endif

  if (impl == nullptr) {
    impl = new demod_engine_impl();
  }

  std::lock_guard<std::mutex> lock(demod_resources_mutex());
  demod_resources_t& res = demod_resources();
  if (res.device == nil) {
    res.device = MTLCreateSystemDefaultDevice();
    if (res.device == nil) {
      ocudulog::fetch_basic_logger("PHY").error("Metal demapper: no Metal device available");
      return false;
    }
    res.queue = metal::shared_queue::backend_queue();
    NSString* lib_path = resolve_demod_metallib_path();
    if (lib_path == nil) {
      ocudulog::fetch_basic_logger("PHY").error(
          "Metal demapper: pre-compiled shader library 'ocudu_demod.metallib' not found");
      return false;
    }
    NSError*       error   = nil;
    id<MTLLibrary> library = [res.device newLibraryWithURL:[NSURL fileURLWithPath:lib_path] error:&error];
    if (library == nil) {
      ocudulog::fetch_basic_logger("PHY").error("Metal demapper: failed to load the shader library {}: {}",
                                                lib_path.UTF8String,
                                                error != nil ? error.localizedDescription.UTF8String : "nil error");
      return false;
    }
    id<MTLFunction> fn = [library newFunctionWithName:@"demod_soft"];
    if (fn == nil) {
      ocudulog::fetch_basic_logger("PHY").error("Metal demapper: kernel 'demod_soft' not found");
      return false;
    }
    res.pipeline = [res.device newComputePipelineStateWithFunction:fn error:&error];
    if (res.pipeline == nil) {
      ocudulog::fetch_basic_logger("PHY").error("Metal demapper: pipeline creation failed: {}",
                                                error != nil ? error.localizedDescription.UTF8String : "nil error");
      return false;
    }
    ocudulog::fetch_basic_logger("PHY").debug("Metal demapper: loaded pre-compiled shader library {}",
                                              lib_path.UTF8String);
  }
  return true;
}

bool demod_metal_engine::begin_batch()
{
  demod_engine_impl* engine = static_cast<demod_engine_impl*>(impl);
  if (engine == nullptr || demod_resources().pipeline == nil || engine->batch_cb != nil) {
    return false;
  }
  engine->batch_cb           = [demod_resources().queue commandBuffer];
  engine->batch_enc          = [engine->batch_cb computeCommandEncoder];
  engine->batch_n            = 0;
  [engine->batch_enc setComputePipelineState:demod_resources().pipeline];
  return engine->batch_enc != nil;
}

bool demod_metal_engine::enqueue(const void* symbols,
                                 const void* noise_var,
                                 void*       llrs,
                                 unsigned    nof_symbols,
                                 unsigned    mod)
{
  demod_engine_impl* engine = static_cast<demod_engine_impl*>(impl);
  if (engine == nullptr || engine->batch_enc == nil) {
    return false;
  }
  const size_t symbols_bytes = static_cast<size_t>(nof_symbols) * 2 * sizeof(float);
  const size_t noise_bytes   = static_cast<size_t>(nof_symbols) * sizeof(float);
  const size_t llr_bytes     = static_cast<size_t>(nof_symbols) * 8;
  // wrap_buffer() clears this flag when a no-copy wrap falls back to a copy. Reset it before the
  // wraps (not after, where it would overwrite the outcome) so the diagnostic reports the truth.
  engine->last_call_no_copy = true;
  id<MTLBuffer> b_sym  = wrap_buffer(engine, symbols, symbols_bytes);
  id<MTLBuffer> b_nv   = wrap_buffer(engine, noise_var, noise_bytes);
  id<MTLBuffer> b_llrs = wrap_buffer(engine, llrs, llr_bytes);
  if (b_sym == nil || b_nv == nil || b_llrs == nil) {
    return false;
  }
  const demod_params_t params{nof_symbols, mod};
  id<MTLComputeCommandEncoder> enc = engine->batch_enc;
  [enc setBuffer:b_sym offset:0 atIndex:0];
  [enc setBuffer:b_nv offset:0 atIndex:1];
  [enc setBuffer:b_llrs offset:0 atIndex:2];
  [enc setBytes:&params length:sizeof(params) atIndex:3];
  [enc dispatchThreads:MTLSizeMake(nof_symbols, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
  ++engine->batch_n;
  return true;
}

bool demod_metal_engine::batch_open() const
{
  const demod_engine_impl* engine = static_cast<const demod_engine_impl*>(impl);
  return (engine != nullptr) && (engine->batch_cb != nil);
}

bool demod_metal_engine::enqueue_burst(const void* symbols,
                                       const void* noise_var,
                                       void*       llrs,
                                       unsigned    nof_symbols,
                                       unsigned    mod)
{
  demod_engine_impl* engine = static_cast<demod_engine_impl*>(impl);
  if (engine == nullptr) {
    return false;
  }
  // The encoder switches the compute pipeline, which inserts the memory barrier that orders this
  // stage after the equalization encoded before it in the same command buffer.
  id<MTLComputeCommandEncoder> enc = metal::shared_burst::encoder(demod_resources().pipeline);
  if (enc == nil) {
    return false;
  }
  const size_t symbols_bytes = static_cast<size_t>(nof_symbols) * 2 * sizeof(float);
  const size_t noise_bytes   = static_cast<size_t>(nof_symbols) * sizeof(float);
  const size_t llr_bytes     = static_cast<size_t>(nof_symbols) * 8;

  engine->last_call_no_copy = true;
  id<MTLBuffer> b_sym  = wrap_buffer(engine, symbols, symbols_bytes);
  id<MTLBuffer> b_nv   = wrap_buffer(engine, noise_var, noise_bytes);
  id<MTLBuffer> b_llrs = wrap_buffer(engine, llrs, llr_bytes);
  if (b_sym == nil || b_nv == nil || b_llrs == nil) {
    return false;
  }
  const demod_params_t params{nof_symbols, mod};
  [enc setBuffer:b_sym offset:0 atIndex:0];
  [enc setBuffer:b_nv offset:0 atIndex:1];
  [enc setBuffer:b_llrs offset:0 atIndex:2];
  [enc setBytes:&params length:sizeof(params) atIndex:3];
  [enc dispatchThreads:MTLSizeMake(nof_symbols, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
  metal::shared_burst::count_dispatch(metal::shared_burst::stage::demapper);
  return true;
}

bool demod_metal_engine::burst_open()
{
  return metal::shared_burst::open();
}

bool demod_metal_engine::burst_commit()
{
  return metal::shared_burst::commit();
}

bool demod_metal_engine::burst_wait_committed()
{
  return metal::shared_burst::wait_committed();
}

bool demod_metal_engine::commit_batch()
{
  demod_engine_impl* engine = static_cast<demod_engine_impl*>(impl);
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
  demod_stats_commit();
  engine->outstanding.push_back(cmd_buf);
  return true;
}

bool demod_metal_engine::wait_committed()
{
  demod_engine_impl* engine = static_cast<demod_engine_impl*>(impl);
  if ((engine == nullptr) || engine->outstanding.empty()) {
    return true;
  }
  std::vector<id<MTLCommandBuffer>> outstanding;
  outstanding.swap(engine->outstanding);
  bool ok = true;
  for (id<MTLCommandBuffer> cmd_buf : outstanding) {
    demod_stats_wait();
    [cmd_buf waitUntilCompleted];
    if (cmd_buf.status != MTLCommandBufferStatusCompleted) {
      ocudulog::fetch_basic_logger("PHY").error("Metal demapper: command buffer failed with status {}",
                                                static_cast<unsigned long>(cmd_buf.status));
      ok = false;
    }
    if (cmd_buf.GPUStartTime > 0.0 && cmd_buf.GPUEndTime > 0.0) {
      engine->last_gpu_us = (cmd_buf.GPUEndTime - cmd_buf.GPUStartTime) * 1e6;
    }
  }
  return ok;
}

bool demod_metal_engine::demodulate(const void* symbols,
                                    const void* noise_var,
                                    void*       llrs,
                                    unsigned    nof_symbols,
                                    unsigned    mod)
{
  // Compatibility wrapper: one dispatch per command buffer, waited immediately.
  if (!begin_batch()) {
    return false;
  }
  if (!enqueue(symbols, noise_var, llrs, nof_symbols, mod)) {
    (void)commit_batch();
    return false;
  }
  if (!commit_batch()) {
    return false;
  }
  return wait_committed();
}

bool demod_metal_engine::last_call_used_no_copy() const
{
  const demod_engine_impl* engine = static_cast<const demod_engine_impl*>(impl);
  return engine != nullptr ? engine->last_call_no_copy : false;
}

double demod_metal_engine::last_gpu_wait_us() const
{
  const demod_engine_impl* engine = static_cast<const demod_engine_impl*>(impl);
  return engine != nullptr ? engine->last_gpu_us : 0.0;
}

} // namespace metal
} // namespace ocudu
