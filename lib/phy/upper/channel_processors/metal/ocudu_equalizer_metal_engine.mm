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
#include <cstring>
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
  /// First element of the channel estimates, in cbf16_t elements (see ch_est_binding).
  uint32_t h_offset;
  /// Elements between two consecutive transmission layers.
  uint32_t h_layer_stride;
};

/// Bytes the kernel may read from a channel-estimate binding: its offset plus every port and layer
/// it holds, stride included.
static size_t ch_est_binding_bytes(const equalizer_metal_engine::ch_est_binding& h,
                                   unsigned                                      nof_ports,
                                   unsigned                                      nof_layers,
                                   unsigned                                      nof_re)
{
  const size_t stride = (h.layer_stride != 0) ? h.layer_stride : nof_re;
  return (static_cast<size_t>(h.offset) + static_cast<size_t>(nof_ports) * nof_layers * stride) * sizeof(cbf16_t);
}

/// Kernel parameters of one dispatch: the binding carries its own layout (see ch_est_binding_bytes).
static equalize_params_t make_params(const equalizer_metal_engine::ch_est_binding& h,
                                     unsigned                                      nof_re,
                                     unsigned                                      nof_ports,
                                     unsigned                                      nof_layers,
                                     bool                                          mmse,
                                     float                                         noise_var,
                                     float                                         tx_scaling,
                                     float                                         h_scaling)
{
  return equalize_params_t{nof_re,
                           nof_ports,
                           nof_layers,
                           mmse ? 1u : 0u,
                           noise_var,
                           tx_scaling,
                           h_scaling,
                           h.offset,
                           (h.layer_stride != 0) ? h.layer_stride : nof_re};
}

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

struct wrapped_buffer {
  id<MTLBuffer> buffer = nil;
  NSUInteger    offset = 0;
};

wrapped_buffer wrap_buffer(eq_engine_impl* engine, const void* ptr, size_t length)
{
  // One buffer object per address for every engine: the stages of the chain write and read the same
  // memory, and Metal only relates accesses through the resource they are bound to (see
  // shared_queue::wrap_no_copy).
  size_t        offset = 0;
  id<MTLBuffer> buf    = metal::shared_queue::wrap_no_copy(metal::shared_queue::device(), ptr, length, &offset);
  if (buf != nil) {
    return wrapped_buffer{buf, static_cast<NSUInteger>(offset)};
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
  return wrapped_buffer{
      [metal::shared_queue::device() newBufferWithBytes:ptr length:length options:MTLResourceStorageModeShared], 0};
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

bool equalizer_metal_engine::enqueue(const ch_est_binding& h,
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
  const size_t h_bytes = ch_est_binding_bytes(h, nof_ports, nof_layers, nof_re);
  const size_t y_bytes = static_cast<size_t>(nof_ports) * nof_re * 4;
  const size_t s_bytes = static_cast<size_t>(nof_ports) * sizeof(float);
  const size_t eq_bytes = static_cast<size_t>(nof_layers) * nof_re * 2 * sizeof(float);
  const size_t nv_bytes = static_cast<size_t>(nof_layers) * nof_re * sizeof(float);
  // wrap_buffer() clears this flag when a no-copy wrap falls back to a copy. Reset it before the
  // wraps (not after, where it would overwrite the outcome) so the diagnostic reports the truth.
  engine->last_call_no_copy = true;
  wrapped_buffer b_h = wrap_buffer(engine, h.buffer, h_bytes);
  wrapped_buffer b_y = wrap_buffer(engine, y, y_bytes);
  wrapped_buffer b_s = wrap_buffer(engine, sigma2, s_bytes);
  wrapped_buffer b_eq = wrap_buffer(engine, eq, eq_bytes);
  wrapped_buffer b_nv = wrap_buffer(engine, nv, nv_bytes);
  if (b_h.buffer == nil || b_y.buffer == nil || b_s.buffer == nil || b_eq.buffer == nil || b_nv.buffer == nil) {
    return false;
  }
  const equalize_params_t params =
      make_params(h, nof_re, nof_ports, nof_layers, mmse, noise_var, tx_scaling, h_scaling);
  id<MTLComputeCommandEncoder> enc = engine->batch_enc;
  [enc setBuffer:b_h.buffer offset:b_h.offset atIndex:0];
  [enc setBuffer:b_y.buffer offset:b_y.offset atIndex:1];
  [enc setBuffer:b_eq.buffer offset:b_eq.offset atIndex:2];
  [enc setBuffer:b_nv.buffer offset:b_nv.offset atIndex:3];
  [enc setBuffer:b_s.buffer offset:b_s.offset atIndex:5];
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

/// One equalization the deferred path accumulated instead of encoding one dispatch per symbol.
struct eq_pending_t {
  equalizer_metal_engine::ch_est_binding h;
  const void* y         = nullptr;
  const void* sigma2    = nullptr;
  void*       eq        = nullptr;
  void*       nv        = nullptr;
  unsigned    nof_re    = 0;
  unsigned    nof_ports = 0;
  unsigned    nof_layers = 0;
  bool        mmse      = false;
  float       noise_var = 0.0F;
  float       tx_scaling = 0.0F;
  float       h_scaling  = 0.0F;
};

/// Per-thread accumulation of the deferred burst (the burst itself is thread local).
///
/// The equalizer submits one symbol per call, which used to encode one dispatch per symbol. The
/// dispatches are accumulated here instead and handed over through shared_burst's flush hook right
/// before the burst changes stage or commits, so a group of symbols whose geometry, noise path and
/// output strides match becomes ONE batched dispatch - without the caller having to collect
/// anything (the inputs are staged per symbol, exactly as before).
struct eq_flush_state_t {
  void*                      engine = nullptr;
  std::vector<eq_pending_t>  pending;
  /// Group staging handed over to a committed command buffer, recycled by the next flush (the
  /// kernels read it until that command buffer completes).
  std::vector<void*>         inflight;
};
static eq_flush_state_t& eq_flush_state()
{
  static thread_local eq_flush_state_t s;
  return s;
}

/// Recycles the group staging of the previous flush (its command buffer was waited for).
static void eq_flush_recycle()
{
  eq_flush_state_t& st = eq_flush_state();
  for (void* p : st.inflight) {
    compat::aligned_free(p);
  }
  st.inflight.clear();
}

/// Encodes every accumulated symbol into the open burst: one batched dispatch per run of symbols
/// that share geometry, noise path, scaling and output strides, and the per-symbol kernel for the
/// runs that do not. Returns the pipeline the dispatches were encoded with.
static id<MTLComputePipelineState> eq_flush_hook(void* context, id<MTLComputeCommandEncoder> enc)
{
  eq_flush_state_t& st = eq_flush_state();
  if ((enc == nil) || (st.engine != context) || st.pending.empty()) {
    return nil;
  }
  eq_engine_impl* engine = static_cast<eq_engine_impl*>(context);
  if (engine == nullptr) {
    return nil;
  }
  // The previous flush's command buffer has been waited for before a new group is accumulated.
  eq_flush_recycle();

  const unsigned n_sym = static_cast<unsigned>(st.pending.size());
  unsigned       first = 0;
  bool           any_batch = false;
  id<MTLComputePipelineState> used_pipeline = nil;
  while (first != n_sym) {
    const eq_pending_t& head = st.pending[first];
    // Extend the run while geometry, noise path, scalings, noise variances and output strides match.
    unsigned n_run = 1;
    while (first + n_run != n_sym) {
      const eq_pending_t& next = st.pending[first + n_run];
      const eq_pending_t& prev = st.pending[first + n_run - 1];
      const bool same_geom = (next.nof_re == head.nof_re) && (next.nof_ports == head.nof_ports) &&
                             (next.nof_layers == head.nof_layers) && (next.mmse == head.mmse) &&
                             (next.noise_var == head.noise_var) && (next.tx_scaling == head.tx_scaling) &&
                             (next.h_scaling == head.h_scaling);
      // The group staging below copies the estimates of every symbol of the run, so a run can only
      // hold symbols whose estimates are laid out the same way. That includes the device slices of
      // K3: two symbols of one hop share a buffer but not their offset.
      const bool same_h = (next.h.buffer == head.h.buffer) && (next.h.layer_stride == head.h.layer_stride);
      const bool same_strides =
          (static_cast<const char*>(next.eq) - static_cast<const char*>(prev.eq)) ==
              (static_cast<const char*>(st.pending[first + 1].eq) - static_cast<const char*>(head.eq)) &&
          (static_cast<const char*>(next.nv) - static_cast<const char*>(prev.nv)) ==
              (static_cast<const char*>(st.pending[first + 1].nv) - static_cast<const char*>(head.nv)) &&
          (next.eq != nullptr) && (next.nv != nullptr);
      const bool same_sigma = (std::memcmp(next.sigma2, head.sigma2, head.nof_ports * sizeof(float)) == 0);
      if (!same_geom || !same_strides || !same_sigma || !same_h) {
        break;
      }
      ++n_run;
    }

    // Group staging: h [symbol][port][layer][re], y [symbol][port][re], one sigma2 array per run.
    const size_t h_stride = static_cast<size_t>(head.nof_ports) * head.nof_layers * head.nof_re;
    const size_t y_stride = static_cast<size_t>(head.nof_ports) * head.nof_re;
    const unsigned eq_stride_elems =
        (n_run > 1) ? static_cast<unsigned>((static_cast<const char*>(st.pending[first + 1].eq) -
                                             static_cast<const char*>(head.eq)) /
                                            2 / sizeof(float))
                    : 0;
    const unsigned nv_stride_elems =
        (n_run > 1) ? static_cast<unsigned>((static_cast<const char*>(st.pending[first + 1].nv) -
                                             static_cast<const char*>(head.nv)) /
                                            sizeof(float))
                    : 0;
    auto* h_alloc = static_cast<cbf16_t*>(compat::aligned_alloc(compat::page_size(), h_stride * n_run * sizeof(cbf16_t)));
    auto* y_alloc = static_cast<cbf16_t*>(compat::aligned_alloc(compat::page_size(), y_stride * n_run * sizeof(cbf16_t)));
    auto* s_alloc = static_cast<float*>(compat::aligned_alloc(compat::page_size(), head.nof_ports * sizeof(float)));
    if ((h_alloc == nullptr) || (y_alloc == nullptr) || (s_alloc == nullptr)) {
      compat::aligned_free(h_alloc);
      compat::aligned_free(y_alloc);
      compat::aligned_free(s_alloc);
      return nil;
    }
    for (unsigned k = 0; k != n_run; ++k) {
      std::memcpy(h_alloc + k * h_stride,
                  static_cast<const cbf16_t*>(st.pending[first + k].h.buffer) + st.pending[first + k].h.offset,
                  h_stride * sizeof(cbf16_t));
      std::memcpy(y_alloc + k * y_stride, st.pending[first + k].y, y_stride * sizeof(cbf16_t));
    }
    const equalize_params_t params = make_params(equalizer_metal_engine::ch_est_binding(h_alloc),
                                                 head.nof_re,
                                                 head.nof_ports,
                                                 head.nof_layers,
                                                 head.mmse,
                                                 head.noise_var,
                                                 head.tx_scaling,
                                                 head.h_scaling);
    const eq_strides_t strides{n_run,
                               static_cast<unsigned>(h_stride),
                               static_cast<unsigned>(y_stride),
                               eq_stride_elems,
                               nv_stride_elems};
    std::memcpy(s_alloc, head.sigma2, head.nof_ports * sizeof(float));



    const size_t h_bytes  = h_stride * n_run * sizeof(cbf16_t);
    const size_t y_bytes  = y_stride * n_run * sizeof(cbf16_t);
    const size_t s_bytes  = static_cast<size_t>(head.nof_ports) * sizeof(float);
    const size_t eq_bytes = ((static_cast<size_t>(n_run) - 1) * eq_stride_elems + static_cast<size_t>(head.nof_re) * head.nof_layers) * 2 * sizeof(float);
    const size_t nv_bytes = ((static_cast<size_t>(n_run) - 1) * nv_stride_elems + static_cast<size_t>(head.nof_re) * head.nof_layers) * sizeof(float);

    wrapped_buffer b_h = wrap_buffer(engine, h_alloc, h_bytes);
    wrapped_buffer b_y = wrap_buffer(engine, y_alloc, y_bytes);
    wrapped_buffer b_s = wrap_buffer(engine, s_alloc, s_bytes);
    wrapped_buffer b_eq = wrap_buffer(engine, head.eq, eq_bytes);
    wrapped_buffer b_nv = wrap_buffer(engine, head.nv, nv_bytes);
    if ((b_h.buffer == nil) || (b_y.buffer == nil) || (b_s.buffer == nil) || (b_eq.buffer == nil) || (b_nv.buffer == nil)) {
      engine->last_call_no_copy = false;
      compat::aligned_free(h_alloc);
      compat::aligned_free(y_alloc);
      compat::aligned_free(s_alloc);
      return nil;
    }
    engine->last_call_no_copy = true;

    // A single-symbol run keeps the per-symbol kernel: it is the path every caller already
    // validates, while the batched kernel is the one that has to prove itself with a group.
    id<MTLComputePipelineState> run_pipeline = (n_run > 1) ? eq_resources().pipeline_batch : eq_resources().pipeline;
    used_pipeline                            = run_pipeline;
    [enc setComputePipelineState:run_pipeline];
    [enc setBuffer:b_h.buffer offset:b_h.offset atIndex:0];
    [enc setBuffer:b_y.buffer offset:b_y.offset atIndex:1];
    [enc setBuffer:b_eq.buffer offset:b_eq.offset atIndex:2];
    [enc setBuffer:b_nv.buffer offset:b_nv.offset atIndex:3];
    [enc setBytes:&params length:sizeof(params) atIndex:4];
    [enc setBuffer:b_s.buffer offset:b_s.offset atIndex:5];
    if (n_run > 1) {
      [enc setBytes:&strides length:sizeof(strides) atIndex:6];
    }
    [enc dispatchThreads:MTLSizeMake(head.nof_re, n_run, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
    metal::shared_burst::count_dispatch(metal::shared_burst::stage::equalizer);
    if (n_run > 1) {
      ++engine->batch_dispatches;
    }

    // The group staging stays alive until the command buffer that reads it completes: it is
    // recycled by the next flush, which the caller only reaches after waiting (see wait()).
    st.inflight.push_back(h_alloc);
    st.inflight.push_back(y_alloc);
    st.inflight.push_back(s_alloc);
    any_batch = true;
    first += n_run;
  }

  st.pending.clear();
  (void)any_batch;
  return used_pipeline;
}

bool equalizer_metal_engine::enqueue_burst(const ch_est_binding& h,
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
  // Accumulate instead of encoding one dispatch per symbol: the dispatches of the whole group are
  // encoded by the flush hook when the burst changes stage (the demapping) or commits, so symbols
  // that share geometry, noise path and output strides become one batched dispatch. The caller's
  // inputs are read at that point, which is why they must stay alive until wait() - exactly the
  // contract the per-symbol path already had.
  eq_flush_state_t& st = eq_flush_state();
  if ((st.engine != nullptr) && (st.engine != engine) && !st.pending.empty()) {
    // A different engine left work pending in this thread's burst: hand it over before mixing.
    metal::shared_burst::set_flush_hook(st.engine, &eq_flush_hook);
    (void)metal::shared_burst::encoder(eq_resources().pipeline);
  }
  // Default: encode the dispatch right here, exactly as the chain has always done (verified: a
  // deferred group of any size matches the synchronous path bit for bit). The accumulated/flushed
  // form below - which is what turns a group of symbols into ONE batched dispatch - is selected by
  // OCUDU_EQ_DEFER_ENCODE=1 and is NOT equivalent yet: encoding the equalization later (at the
  // first encode of the next stage) makes the demapping read symbols >= 2 of a group as if the
  // equalization had not run (their soft bits come out zero), even with the batching disabled.
  // The measurement that separates the two: identical plumbing with immediate encoding = 0 of 4896
  // differing LLRs, deferred encoding = 4485, and forcing one dispatch per symbol while deferred
  // still differs (1442 for a group of two) - so the defect is the ENCODING INSTANT, not the
  // batched dispatch. See the S-5 section of the full-chain design document.
  if (std::getenv("OCUDU_EQ_DEFER_ENCODE") == nullptr) {
    id<MTLComputeCommandEncoder> enc = metal::shared_burst::encoder(eq_resources().pipeline);
    if (enc == nil) {
      return false;
    }
    const size_t h_bytes  = ch_est_binding_bytes(h, nof_ports, nof_layers, nof_re);
    const size_t y_bytes  = static_cast<size_t>(nof_ports) * nof_re * sizeof(cbf16_t);
    const size_t s_bytes  = static_cast<size_t>(nof_ports) * sizeof(float);
    const size_t eq_bytes = static_cast<size_t>(nof_layers) * nof_re * 2 * sizeof(float);
    const size_t nv_bytes = static_cast<size_t>(nof_layers) * nof_re * sizeof(float);
    wrapped_buffer b_h = wrap_buffer(engine, h.buffer, h_bytes);
    wrapped_buffer b_y = wrap_buffer(engine, y, y_bytes);
    wrapped_buffer b_s = wrap_buffer(engine, sigma2, s_bytes);
    wrapped_buffer b_eq = wrap_buffer(engine, eq, eq_bytes);
    wrapped_buffer b_nv = wrap_buffer(engine, nv, nv_bytes);
    if (b_h.buffer == nil || b_y.buffer == nil || b_s.buffer == nil || b_eq.buffer == nil || b_nv.buffer == nil) {
      engine->last_call_no_copy = false;
      return false;
    }
    engine->last_call_no_copy         = true;
    const equalize_params_t params =
        make_params(h, nof_re, nof_ports, nof_layers, mmse, noise_var, tx_scaling, h_scaling);
    [enc setBuffer:b_h.buffer offset:b_h.offset atIndex:0];
    [enc setBuffer:b_y.buffer offset:b_y.offset atIndex:1];
    [enc setBuffer:b_eq.buffer offset:b_eq.offset atIndex:2];
    [enc setBuffer:b_nv.buffer offset:b_nv.offset atIndex:3];
    [enc setBytes:&params length:sizeof(params) atIndex:4];
    [enc setBuffer:b_s.buffer offset:b_s.offset atIndex:5];
    [enc dispatchThreads:MTLSizeMake(nof_re, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
    metal::shared_burst::count_dispatch(metal::shared_burst::stage::equalizer);
    return true;
  }
  st.engine = engine;
  st.pending.push_back({h, y, sigma2, eq, nv, nof_re, nof_ports, nof_layers, mmse, noise_var, tx_scaling, h_scaling});
  metal::shared_burst::set_flush_hook(engine, &eq_flush_hook);
  return true;
}

bool equalizer_metal_engine::enqueue_burst_batch(const ch_est_binding& h,
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
                          ((static_cast<size_t>(nof_symbols) - 1) * h_symbol_stride + nof_re) * sizeof(cbf16_t) +
                          static_cast<size_t>(h.offset) * sizeof(cbf16_t);
  const size_t y_bytes  = static_cast<size_t>(nof_ports) *
                          ((static_cast<size_t>(nof_symbols) - 1) * y_symbol_stride + nof_re) * sizeof(cbf16_t);
  const size_t s_bytes  = static_cast<size_t>(nof_ports) * sizeof(float);
  const size_t eq_bytes = ((static_cast<size_t>(nof_symbols) - 1) * eq_symbol_stride + nof_re * nof_layers) * 2 *
                          sizeof(float);
  const size_t nv_bytes = ((static_cast<size_t>(nof_symbols) - 1) * nv_symbol_stride + nof_re * nof_layers) *
                          sizeof(float);

  wrapped_buffer b_h = wrap_buffer(engine, h.buffer, h_bytes);
  wrapped_buffer b_y = wrap_buffer(engine, y, y_bytes);
  wrapped_buffer b_s = wrap_buffer(engine, sigma2, s_bytes);
  wrapped_buffer b_eq = wrap_buffer(engine, eq, eq_bytes);
  wrapped_buffer b_nv = wrap_buffer(engine, nv, nv_bytes);
  if (b_h.buffer == nil || b_y.buffer == nil || b_s.buffer == nil || b_eq.buffer == nil || b_nv.buffer == nil) {
    engine->last_call_no_copy = false;
    return false;
  }
  engine->last_call_no_copy          = true;
  const equalize_params_t params =
      make_params(h, nof_re, nof_ports, nof_layers, mmse, noise_var, tx_scaling, h_scaling);
  const eq_strides_t      strides{nof_symbols, h_symbol_stride, y_symbol_stride, eq_symbol_stride, nv_symbol_stride};
  [enc setBuffer:b_h.buffer offset:b_h.offset atIndex:0];
  [enc setBuffer:b_y.buffer offset:b_y.offset atIndex:1];
  [enc setBuffer:b_eq.buffer offset:b_eq.offset atIndex:2];
  [enc setBuffer:b_nv.buffer offset:b_nv.offset atIndex:3];
  [enc setBytes:&params length:sizeof(params) atIndex:4];
  [enc setBuffer:b_s.buffer offset:b_s.offset atIndex:5];
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

bool equalizer_metal_engine::equalize(const ch_est_binding& h,
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
