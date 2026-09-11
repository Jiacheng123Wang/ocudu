// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu_equalizer_metal_engine.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "ocudu/ocudulog/ocudulog.h"

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
               "[metal_stats] equalizer commits=%llu waits=%llu max_in_flight=%llu\n",
               static_cast<unsigned long long>(s.commits.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.waits.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.in_flight_max.load(std::memory_order_relaxed)));
}
#else
static void eq_stats_commit() {}
static void eq_stats_wait() {}
#endif // OCUDU_METAL_STATS

struct eq_resources_t {
  id<MTLDevice>               device   = nil;
  id<MTLCommandQueue>         queue    = nil;
  id<MTLComputePipelineState> pipeline = nil;
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
};

struct eq_engine_impl {
  double last_gpu_us = 0.0;
  std::unordered_map<const void*, std::pair<id<MTLBuffer>, size_t>> buffer_cache;
};

id<MTLBuffer> wrap_buffer(eq_engine_impl* engine, const void* ptr, size_t length)
{
  auto it = engine->buffer_cache.find(ptr);
  if (it != engine->buffer_cache.end()) {
    if (length <= it->second.second) {
      return it->second.first;
    }
    ocudulog::fetch_basic_logger("PHY").warning(
        "Metal equalizer: zero-copy cache hit with a larger request ({} > cached {}): re-wrapping the buffer",
        length,
        it->second.second);
  }
  const size_t aligned = (length + 4095) & ~4095;
  id<MTLBuffer> buf    = [eq_resources().device newBufferWithBytesNoCopy:(void*)ptr
                                                              length:aligned
                                                             options:MTLResourceStorageModeShared
                                                         deallocator:nil];
  if (buf == nil) {
    buf = [eq_resources().device newBufferWithBytes:ptr length:length options:MTLResourceStorageModeShared];
  }
  engine->buffer_cache[ptr] = std::make_pair(buf, aligned);
  return buf;
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
    res.queue = [res.device newCommandQueue];
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
                                      float       tx_scaling)
{
  eq_engine_impl* engine = static_cast<eq_engine_impl*>(impl);
  if (engine == nullptr || eq_resources().pipeline == nil) {
    return false;
  }
  const size_t h_bytes  = static_cast<size_t>(nof_ports) * nof_layers * nof_re * 2 * sizeof(float);
  const size_t y_bytes  = static_cast<size_t>(nof_ports) * nof_re * 2 * sizeof(float);
  const size_t s_bytes  = static_cast<size_t>(nof_ports) * sizeof(float);
  const size_t eq_bytes = static_cast<size_t>(nof_layers) * nof_re * 2 * sizeof(float);
  const size_t nv_bytes = static_cast<size_t>(nof_layers) * nof_re * sizeof(float);
  id<MTLBuffer> b_h  = wrap_buffer(engine, h, h_bytes);
  id<MTLBuffer> b_y  = wrap_buffer(engine, y, y_bytes);
  id<MTLBuffer> b_s  = wrap_buffer(engine, sigma2, s_bytes);
  id<MTLBuffer> b_eq = wrap_buffer(engine, eq, eq_bytes);
  id<MTLBuffer> b_nv = wrap_buffer(engine, nv, nv_bytes);
  if (b_h == nil || b_y == nil || b_s == nil || b_eq == nil || b_nv == nil) {
    return false;
  }

  equalize_params_t params{nof_re, nof_ports, nof_layers, mmse ? 1u : 0u, noise_var, tx_scaling};

  id<MTLCommandBuffer>         cmd_buf = [eq_resources().queue commandBuffer];
  id<MTLComputeCommandEncoder> enc     = [cmd_buf computeCommandEncoder];
  [enc setComputePipelineState:eq_resources().pipeline];
  [enc setBuffer:b_h offset:0 atIndex:0];
  [enc setBuffer:b_y offset:0 atIndex:1];
  [enc setBuffer:b_eq offset:0 atIndex:2];
  [enc setBuffer:b_nv offset:0 atIndex:3];
  [enc setBuffer:b_s offset:0 atIndex:5];
  [enc setBytes:&params length:sizeof(params) atIndex:4];
  [enc dispatchThreads:MTLSizeMake(nof_re, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
  [enc endEncoding];
  [cmd_buf commit];
  eq_stats_commit();
  [cmd_buf waitUntilCompleted];
  eq_stats_wait();
  if (cmd_buf.status != MTLCommandBufferStatusCompleted) {
    ocudulog::fetch_basic_logger("PHY").error("Metal equalizer: command buffer failed with status {}",
                                              static_cast<unsigned long>(cmd_buf.status));
    return false;
  }
  if (cmd_buf.GPUStartTime > 0.0 && cmd_buf.GPUEndTime > 0.0) {
    engine->last_gpu_us = (cmd_buf.GPUEndTime - cmd_buf.GPUStartTime) * 1e6;
  } else {
    engine->last_gpu_us = 0.0;
  }
  return true;
}

double equalizer_metal_engine::last_gpu_wait_us() const
{
  const eq_engine_impl* engine = static_cast<const eq_engine_impl*>(impl);
  return engine != nullptr ? engine->last_gpu_us : 0.0;
}

} // namespace metal
} // namespace ocudu
