// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu_dft_metal_engine.h"
#include "ocudu/support/executors/phy_shutdown_report.h"

#include "ocudu_metal_queue.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "ocudu/ocudulog/ocudulog.h"

#include "ocudu/support/macos_compat.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

#ifndef OCUDU_DFT_METALLIB_PATH
#define OCUDU_DFT_METALLIB_PATH "ocudu_dft.metallib"
#endif

using namespace ocudu;

namespace ocudu {
namespace metal {

namespace {

// ---- Process-wide dispatch/wait statistics (same accounting as the LDPC/MMSE engines) ----
// Compile-time debug aid (ENABLE_METAL_STATS=ON defines OCUDU_METAL_STATS); off by default
// with zero overhead. Reported at process exit.
#if defined(OCUDU_METAL_STATS)
struct dft_stats_t {
  std::atomic<uint64_t> commits{0};
  std::atomic<uint64_t> waits{0};
  std::atomic<uint64_t> in_flight{0};
  std::atomic<uint64_t> in_flight_max{0};
};

static dft_stats_t& dft_stats()
{
  static dft_stats_t s;
  return s;
}

static void dft_stats_commit()
{
  dft_stats_t& s = dft_stats();
  s.commits.fetch_add(1, std::memory_order_relaxed);
  const uint64_t nf = s.in_flight.fetch_add(1, std::memory_order_acq_rel) + 1;
  uint64_t       prev = s.in_flight_max.load(std::memory_order_relaxed);
  while (nf > prev && !s.in_flight_max.compare_exchange_weak(prev, nf, std::memory_order_relaxed)) {
  }
}

static void dft_stats_wait()
{
  dft_stats_t& s = dft_stats();
  s.waits.fetch_add(1, std::memory_order_relaxed);
  s.in_flight.fetch_sub(1, std::memory_order_acq_rel);
}

static void dft_stats_report()
{
  const dft_stats_t& s = dft_stats();
  ocudulog::fetch_basic_logger("PHY").debug("[metal_stats] dft commits={} waits={} max_in_flight={}",
               static_cast<unsigned long long>(s.commits.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.waits.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.in_flight_max.load(std::memory_order_relaxed)));
}
#else  // OCUDU_METAL_STATS
static void dft_stats_commit() {}
static void dft_stats_wait() {}
#endif // OCUDU_METAL_STATS

// ---- Process-wide Metal resources: one device, one queue, one pipeline for all sizes ----
struct dft_resources_t {
  id<MTLDevice>              device   = nil;
  id<MTLCommandQueue>        queue    = nil;
  id<MTLComputePipelineState> pipeline = nil;
};

static dft_resources_t& dft_resources()
{
  static dft_resources_t r;
  return r;
}

static std::mutex& dft_resources_mutex()
{
  static std::mutex m;
  return m;
}

NSString* resolve_dft_metallib_path()
{
  NSMutableArray<NSString*>* candidates = [NSMutableArray arrayWithCapacity:3];
  [candidates addObject:[NSString stringWithUTF8String:OCUDU_DFT_METALLIB_PATH]];
  NSArray<NSString*>* args = [[NSProcessInfo processInfo] arguments];
  if (args.count > 0) {
    [candidates
        addObject:[[args[0] stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"ocudu_dft.metallib"]];
  }
  [candidates addObject:[[[NSFileManager defaultManager] currentDirectoryPath]
                            stringByAppendingPathComponent:@"ocudu_dft.metallib"]];

  NSFileManager* fm = [NSFileManager defaultManager];
  for (NSString* path in candidates) {
    if ([fm fileExistsAtPath:path]) {
      return path;
    }
  }
  return nil;
}

/// Transform slots covered by the input/output buffers (dft_processor_metal::max_batch).
static constexpr unsigned max_batch_slots = 16;

struct dft_engine_impl {
  id<MTLCommandBuffer> last_committed_cb = nil; // newest commit (ring bookkeeping)
  /// Command buffer of the newest submission per transform slot (ring pipelining).
  id<MTLCommandBuffer> slot_cb[max_batch_slots <= 16 ? 16 : max_batch_slots] = {};
  bool                 slot_pending[16]                                  = {};

  uint32_t n       = 0;
  uint32_t radix2  = 0; // number of radix-2 stages (k in N = 2^k * 3^m)
  uint32_t radix3  = 0; // number of radix-3 stages (m)
  uint32_t inverse = 0;
  double   last_gpu_us = 0.0;

  // Zero-copy wrappers, cached by host pointer with the cached length stored alongside
  // (the S-1 audit hardening: a larger request re-wraps instead of silently truncating).
  std::unordered_map<const void*, std::pair<id<MTLBuffer>, size_t>> buffer_cache;

  id<MTLBuffer> buf_tw   = nil; // zero-copy wrap of the host twiddle table (N/2 float2)
  id<MTLBuffer> buf_perm = nil; // zero-copy wrap of the digit-reversal permutation table (N uint32)

  // Optional per-element table of the grid write (the demodulator's DFT window phase compensation), copied into an
  // engine-owned page-aligned buffer so the engine controls its lifetime.
  id<MTLBuffer> buf_window   = nil;
  bool          has_window   = false;
  void*         window_mem   = nullptr;
  size_t        window_bytes = 0;

  // Warm-up scratch (page-aligned, engine lifetime; freed by the destructor).
  void* warmup_mem = nullptr;
};

id<MTLBuffer> wrap_buffer(dft_engine_impl* engine, const void* ptr, size_t length)
{
  auto it = engine->buffer_cache.find(ptr);
  if (it != engine->buffer_cache.end()) {
    if (length <= it->second.second) {
      return it->second.first;
    }
    ocudulog::fetch_basic_logger("PHY").warning(
        "Metal DFT: zero-copy cache hit with a larger request ({} > cached {}): re-wrapping the buffer",
        length,
        it->second.second);
  }
  const size_t aligned = (length + 4095) & ~4095;
  id<MTLBuffer> buf    = [dft_resources().device newBufferWithBytesNoCopy:(void*)ptr
                                                              length:aligned
                                                             options:MTLResourceStorageModeShared
                                                         deallocator:nil];
  if (buf == nil) {
    buf = [dft_resources().device newBufferWithBytes:ptr length:length options:MTLResourceStorageModeShared];
  }
  engine->buffer_cache[ptr] = std::make_pair(buf, aligned);
  return buf;
}

} // namespace

dft_metal_engine::~dft_metal_engine()
{
  dft_engine_impl* engine = static_cast<dft_engine_impl*>(impl);
  if (engine != nullptr) {
    engine->buffer_cache.clear();
    std::free(engine->warmup_mem);
    std::free(engine->window_mem);
    delete engine;
    impl = nullptr;
  }
}

bool dft_metal_engine::init(unsigned size, bool inverse)
{
  // Register the process-exit stats report exactly once (the counters live for the process).
#if defined(OCUDU_METAL_STATS)
  static std::once_flag stats_atexit_flag;
  std::call_once(stats_atexit_flag, []() { ocudu::phy_shutdown_report::add(dft_stats_report); });
#endif

  if (size < 2 || size > max_size) {
    return false;
  }
  // Factor N = 2^k * 3^m (the kernel's supported family); anything else is rejected here
  // (the factory then falls back per configuration).
  {
    unsigned rem = size;
    unsigned k   = 0;
    unsigned m   = 0;
    while (rem % 2 == 0) {
      rem /= 2;
      ++k;
    }
    while (rem % 3 == 0) {
      rem /= 3;
      ++m;
    }
    if (rem != 1) {
      return false;
    }
    auto* engine   = new dft_engine_impl();
    impl           = engine;
    engine->n      = size;
    engine->radix2 = k;
    engine->radix3 = m;
  }
  auto* engine      = static_cast<dft_engine_impl*>(impl);
  engine->inverse   = inverse ? 1u : 0u;

  // Device, queue and pipeline are shared process-wide (they are size-independent).
  {
    std::lock_guard<std::mutex> lock(dft_resources_mutex());
    dft_resources_t& res = dft_resources();
    if (res.device == nil) {
      res.device = MTLCreateSystemDefaultDevice();
      if (res.device == nil) {
        ocudulog::fetch_basic_logger("PHY").error("Metal DFT: no Metal device available");
        delete engine;
        impl = nullptr;
        return false;
      }
      res.queue = metal::shared_queue::queue();

      NSString* lib_path = resolve_dft_metallib_path();
      if (lib_path == nil) {
        ocudulog::fetch_basic_logger("PHY").error(
            "Metal DFT: pre-compiled shader library 'ocudu_dft.metallib' not found (searched the configure-time "
            "path, next to the executable, and the working directory)");
        delete engine;
        impl = nullptr;
        return false;
      }
      NSError*       error   = nil;
      id<MTLLibrary> library = [res.device newLibraryWithURL:[NSURL fileURLWithPath:lib_path] error:&error];
      if (library == nil) {
        ocudulog::fetch_basic_logger("PHY").error("Metal DFT: failed to load the shader library {}: {}",
                                                  lib_path.UTF8String,
                                                  error != nil ? error.localizedDescription.UTF8String : "nil error");
        delete engine;
        impl = nullptr;
        return false;
      }
      id<MTLFunction> fn = [library newFunctionWithName:@"dft_dit"];
      if (fn == nil) {
        ocudulog::fetch_basic_logger("PHY").error("Metal DFT: kernel 'dft_dit' not found in the shader library");
        delete engine;
        impl = nullptr;
        return false;
      }
      res.pipeline = [res.device newComputePipelineStateWithFunction:fn error:&error];
      if (res.pipeline == nil) {
        ocudulog::fetch_basic_logger("PHY").error("Metal DFT: pipeline creation failed: {}",
                                                  error != nil ? error.localizedDescription.UTF8String : "nil error");
        delete engine;
        impl = nullptr;
        return false;
      }

      ocudulog::fetch_basic_logger("PHY").debug("Metal DFT: loaded pre-compiled shader library {}", lib_path.UTF8String);
    }
  }

  // Host-side twiddle table: N/2 entries of exp(-2*pi*i*k/N), page-aligned, zero-copy wrapped.
  const size_t tw_bytes = static_cast<size_t>(size / 2) * 2 * sizeof(float);
  void*        tw_mem   = nullptr;
  if (::posix_memalign(&tw_mem, 4096, tw_bytes) != 0 || tw_mem == nullptr) {
    ocudulog::fetch_basic_logger("PHY").error("Metal DFT: twiddle allocation failed");
    delete engine;
    impl = nullptr;
    return false;
  }
  auto* tw = static_cast<float*>(tw_mem);
  for (uint32_t k = 0; k != size / 2; ++k) {
    const double ang = -2.0 * M_PI * static_cast<double>(k) / static_cast<double>(size);
    tw[2 * k]        = static_cast<float>(std::cos(ang));
    tw[2 * k + 1]    = static_cast<float>(std::sin(ang));
  }
  engine->buf_tw = wrap_buffer(engine, tw_mem, tw_bytes);
  if (engine->buf_tw == nil) {
    ocudulog::fetch_basic_logger("PHY").error("Metal DFT: twiddle buffer wrap failed");
    std::free(tw_mem);
    delete engine;
    impl = nullptr;
    return false;
  }

  // Host-side mixed-radix digit-reversal permutation table (the DIT input order), page-aligned,
  // zero-copy wrapped. Factors are processed radix-2 first, then radix-3, matching the kernel.
  const size_t perm_bytes = static_cast<size_t>(size) * sizeof(uint32_t);
  void*        perm_mem   = nullptr;
  if (::posix_memalign(&perm_mem, 4096, perm_bytes) != 0 || perm_mem == nullptr) {
    ocudulog::fetch_basic_logger("PHY").error("Metal DFT: permutation table allocation failed");
    std::free(tw_mem);
    delete engine;
    impl = nullptr;
    return false;
  }
  {
    auto* perm = static_cast<uint32_t*>(perm_mem);
    for (uint32_t i = 0; i != size; ++i) {
      uint32_t rem        = i;
      uint32_t rev        = 0;
      uint32_t remaining  = size;
      // Radix-2 digits (least significant first).
      for (uint32_t q = 0; q != engine->radix2; ++q) {
        remaining /= 2;
        rev += (rem % 2) * remaining;
        rem /= 2;
      }
      // Radix-3 digits.
      for (uint32_t q = 0; q != engine->radix3; ++q) {
        remaining /= 3;
        rev += (rem % 3) * remaining;
        rem /= 3;
      }
      perm[i] = rev;
    }
  }
  engine->buf_perm = wrap_buffer(engine, perm_mem, perm_bytes);
  if (engine->buf_perm == nil) {
    ocudulog::fetch_basic_logger("PHY").error("Metal DFT: permutation buffer wrap failed");
    std::free(perm_mem);
    std::free(tw_mem);
    delete engine;
    impl = nullptr;
    return false;
  }

  // Warm-up dispatch (once per process): the first command-buffer commit of each pipeline
  // pays the Metal driver's lazy compile; paying it here keeps it off the packet path.
  {
    static std::atomic<int> warmed{0};
    if (warmed.fetch_add(1, std::memory_order_acq_rel) == 0) {
      if (::posix_memalign(&engine->warmup_mem, 4096, static_cast<size_t>(size) * 2 * sizeof(float)) == 0) {
        std::memset(engine->warmup_mem, 0, static_cast<size_t>(size) * 2 * sizeof(float));
        (void)run(engine->warmup_mem, engine->warmup_mem, 1);
      }
    }
  }

  return true;
}

bool dft_metal_engine::submit_slot(const void* in, void* out, unsigned slot)
{
  if (slot >= max_batch_slots) {
    return false;
  }
  bool ok = submit_at(in, out, 1, slot, false);
  if (ok) {
    // Remember the slot's command buffer so a pipelined caller can wait for this transform only
    // (wait_all() would also wait for the newer submissions and flatten the pipeline).
    dft_engine_impl* engine = static_cast<dft_engine_impl*>(impl);
    engine->slot_cb[slot]   = engine->last_committed_cb;
    engine->slot_pending[slot] = true;
  }
  return ok;
}

bool dft_metal_engine::wait_slot(unsigned slot)
{
  dft_engine_impl* engine = static_cast<dft_engine_impl*>(impl);
  if ((engine == nullptr) || (slot >= max_batch_slots) || !engine->slot_pending[slot]) {
    return true;
  }
  id<MTLCommandBuffer> cmd_buf = engine->slot_cb[slot];
  engine->slot_pending[slot]   = false;
  engine->slot_cb[slot]        = nil;
  // Account for the slot wait so [metal_stats] reports the real in-flight depth (the ring keeps
  // up to `pipeline depth` transforms in flight instead of one).
  dft_stats_wait();
  [cmd_buf waitUntilCompleted];
  if (cmd_buf.status != MTLCommandBufferStatusCompleted) {
    ocudulog::fetch_basic_logger("PHY").error("Metal DFT: slot {} command buffer failed with status {}",
                                              slot,
                                              static_cast<unsigned long>(cmd_buf.status));
    return false;
  }
  if (cmd_buf.GPUStartTime > 0.0 && cmd_buf.GPUEndTime > 0.0) {
    engine->last_gpu_us = (cmd_buf.GPUEndTime - cmd_buf.GPUStartTime) * 1e6;
  }
  return true;
}

bool dft_metal_engine::wait_all()
{
  // Every DFT instance commits on the front-end queue and publishes there, so this drains this
  // engine's own work (and every other front-end commit) - not the back-end stages' command
  // buffers, which run on a queue of their own (see shared_queue::queue_kind).
  return metal::shared_queue::wait_all_committed(metal::shared_queue::queue_kind::front_end);
}

bool dft_metal_engine::set_grid_write_window(const void* window, unsigned nof_entries)
{
  dft_engine_impl* engine = static_cast<dft_engine_impl*>(impl);
  if (engine == nullptr) {
    return false;
  }

  // Clear the table (no write applies a window).
  if (window == nullptr || nof_entries == 0) {
    engine->buf_window = nil;
    engine->has_window = false;
    std::free(engine->window_mem);
    engine->window_mem   = nullptr;
    engine->window_bytes = 0;
    return true;
  }

  // The table is constant for the lifetime of the demodulator, so it is copied into an engine-owned page-aligned
  // buffer (a no-copy wrap needs a page-aligned base covering whole pages) and wrapped once.
  const size_t page     = compat::page_size();
  const size_t bytes    = static_cast<size_t>(nof_entries) * 2 * sizeof(float);
  const size_t rounded  = ((bytes + page - 1) / page) * page;
  void*        previous = engine->window_mem;
  void*        mem      = nullptr;
  if (::posix_memalign(&mem, page, rounded) != 0 || mem == nullptr) {
    ocudulog::fetch_basic_logger("PHY").error("Metal DFT: grid-write window allocation failed");
    return false;
  }
  std::memcpy(mem, window, bytes);
  engine->window_mem   = mem;
  engine->window_bytes = rounded;
  std::free(previous);

  engine->buf_window = wrap_buffer(engine, mem, rounded);
  if (engine->buf_window == nil) {
    ocudulog::fetch_basic_logger("PHY").error("Metal DFT: grid-write window buffer wrap failed");
    engine->has_window = false;
    return false;
  }
  engine->has_window = true;
  return true;
}

bool dft_metal_engine::submit_slot_grid_write(const void* in, void* out, unsigned slot, const grid_write& write)
{
  dft_engine_impl* engine = static_cast<dft_engine_impl*>(impl);
  if (engine == nullptr || dft_resources().pipeline == nil) {
    return false;
  }
  if (slot >= max_batch_slots || write.grid_base == nullptr || write.nof_subc == 0) {
    return false;
  }

  // The buffers cover the whole batch (all slots), as in submit_at(): a ring caller reuses them without re-wrapping.
  const size_t  bytes  = static_cast<size_t>(engine->n) * max_batch_slots * 2 * sizeof(float);
  id<MTLBuffer> b_in   = wrap_buffer(engine, in, bytes);
  id<MTLBuffer> b_out  = wrap_buffer(engine, out, bytes);
  id<MTLBuffer> b_grid = wrap_buffer(engine, write.grid_base, write.grid_bytes);
  if (b_in == nil || b_out == nil || b_grid == nil) {
    return false;
  }

  id<MTLCommandBuffer>         cmd_buf = [dft_resources().queue commandBuffer];
  id<MTLComputeCommandEncoder> enc     = [cmd_buf computeCommandEncoder];
  [enc setComputePipelineState:dft_resources().pipeline];
  [enc setBuffer:b_in offset:0 atIndex:0];
  [enc setBuffer:b_out offset:0 atIndex:1];
  [enc setBuffer:engine->buf_tw offset:0 atIndex:2];
  [enc setBuffer:engine->buf_perm offset:0 atIndex:3];
  [enc setBytes:&engine->radix2 length:sizeof(uint32_t) atIndex:4];
  [enc setBytes:&engine->radix3 length:sizeof(uint32_t) atIndex:5];
  [enc setBytes:&engine->inverse length:sizeof(uint32_t) atIndex:6];
  const uint32_t base = slot * engine->n;
  [enc setBytes:&base length:sizeof(uint32_t) atIndex:7];
  // The grid and its per-element table are only read when the write is active; Metal still requires every buffer the
  // kernel names to be bound, so the transform output and the twiddle table stand in when there is none.
  [enc setBuffer:b_grid offset:0 atIndex:8];
  [enc setBuffer:(engine->buf_window != nil ? engine->buf_window : engine->buf_tw) offset:0 atIndex:9];

  struct {
    uint32_t active;
    uint32_t nof_subc;
    uint32_t dst_offset;
    uint32_t map_offset;
    float    phase_re;
    float    phase_im;
    uint32_t apply_window;
    uint32_t pad;
  } params = {1u,
              write.nof_subc,
              write.dst_offset,
              write.map_offset % engine->n,
              write.phase_re,
              write.phase_im,
              (write.apply_window && engine->has_window) ? 1u : 0u,
              0u};
  [enc setBytes:&params length:sizeof(params) atIndex:10];

  [enc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(std::min(engine->n, 1024u), 1, 1)];
  [enc endEncoding];
  [cmd_buf commit];
  dft_stats_commit();
  metal::shared_queue::notify_commit(cmd_buf, metal::shared_queue::queue_kind::front_end);
  engine->last_committed_cb    = cmd_buf;
  engine->slot_cb[slot]        = cmd_buf;
  engine->slot_pending[slot]   = true;
  return true;
}

bool dft_metal_engine::submit_at(
    const void* in, void* out, unsigned nof_transforms, unsigned first_slot, bool wait_for_completion)
{

  dft_engine_impl* engine = static_cast<dft_engine_impl*>(impl);
  if (engine == nullptr || dft_resources().pipeline == nil) {
    return false;
  }

  // The buffers cover the whole batch (all slots): a ring caller reuses them without re-wrapping.
  const size_t bytes      = static_cast<size_t>(engine->n) * max_batch_slots * 2 * sizeof(float);
  const size_t byte_begin = static_cast<size_t>(engine->n) * first_slot * 2 * sizeof(float);
  id<MTLBuffer> b_in  = wrap_buffer(engine, in, bytes);
  id<MTLBuffer> b_out = wrap_buffer(engine, out, bytes);
  (void)byte_begin;
  if (b_in == nil || b_out == nil) {
    return false;
  }

  id<MTLCommandBuffer>         cmd_buf = [dft_resources().queue commandBuffer];
  id<MTLComputeCommandEncoder> enc     = [cmd_buf computeCommandEncoder];
  [enc setComputePipelineState:dft_resources().pipeline];
  [enc setBuffer:b_in offset:0 atIndex:0];
  [enc setBuffer:b_out offset:0 atIndex:1];
  [enc setBuffer:engine->buf_tw offset:0 atIndex:2];
  [enc setBuffer:engine->buf_perm offset:0 atIndex:3];
  [enc setBytes:&engine->radix2 length:sizeof(uint32_t) atIndex:4];
  [enc setBytes:&engine->radix3 length:sizeof(uint32_t) atIndex:5];
  [enc setBytes:&engine->inverse length:sizeof(uint32_t) atIndex:6];
  const uint32_t base = first_slot * engine->n;
  [enc setBytes:&base length:sizeof(uint32_t) atIndex:7];
  [enc dispatchThreadgroups:MTLSizeMake(nof_transforms, 1, 1)
      threadsPerThreadgroup:MTLSizeMake(std::min(engine->n, 1024u), 1, 1)];
  [enc endEncoding];
  [cmd_buf commit];
  dft_stats_commit();
  // Publish the commit on the front-end chain so wait_all_committed() can drain it: the DFT is the
  // only engine on this queue, and it is a different queue than the back-end stages' (a commit must
  // never be published on the wrong chain, or the wait would target another queue's command buffer
  // and return before this one completed).
  metal::shared_queue::notify_commit(cmd_buf, metal::shared_queue::queue_kind::front_end);
  engine->last_committed_cb = cmd_buf;
  if (!wait_for_completion) {
    return true;
  }
  [cmd_buf waitUntilCompleted];
  dft_stats_wait();
  if (cmd_buf.status != MTLCommandBufferStatusCompleted) {
    ocudulog::fetch_basic_logger("PHY").error("Metal DFT: command buffer failed with status {}",
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

bool dft_metal_engine::submit(const void* in, void* out, unsigned nof_transforms)
{
  return submit_at(in, out, nof_transforms, 0, false);
}

bool dft_metal_engine::run(const void* in, void* out, unsigned nof_transforms)
{
  return submit_at(in, out, nof_transforms, 0, true);
}

double dft_metal_engine::last_gpu_wait_us() const
{
  const dft_engine_impl* engine = static_cast<const dft_engine_impl*>(impl);
  return engine != nullptr ? engine->last_gpu_us : 0.0;
}

} // namespace metal
} // namespace ocudu
