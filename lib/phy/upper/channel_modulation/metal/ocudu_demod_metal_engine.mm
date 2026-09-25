// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu_demod_metal_engine.h"
#include "ocudu_metal_burst.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "ocudu_metal_queue.h"

#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/support/macos_compat.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

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
  /// Deferred batch accounting: how many flushes ran, how many symbols they carried and how many
  /// dispatches that took. Same shape as [metal_stats] eq_batch, and the only way to tell from a log
  /// whether the batched encoding really ran (a per-symbol encoding dispatches one per symbol, so
  /// dispatches == symbols means the batching never happened).
  std::atomic<uint64_t> batch_flushes{0};
  std::atomic<uint64_t> batch_symbols{0};
  std::atomic<uint64_t> batch_dispatches{0};
  std::atomic<uint64_t> batch_max_run{0};
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
  std::fprintf(stderr,
               "[metal_stats] demod_batch flushes=%llu symbols=%llu dispatches=%llu max_run=%llu\n",
               static_cast<unsigned long long>(s.batch_flushes.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.batch_symbols.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.batch_dispatches.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.batch_max_run.load(std::memory_order_relaxed)));
}
/// Accounts one flush that encoded \p nof_symbols accumulated symbols as \p nof_dispatches.
static void demod_stats_batch(uint64_t nof_symbols, uint64_t nof_dispatches, uint64_t max_run)
{
  demod_stats_t& s = demod_stats();
  s.batch_flushes.fetch_add(1, std::memory_order_relaxed);
  s.batch_symbols.fetch_add(nof_symbols, std::memory_order_relaxed);
  s.batch_dispatches.fetch_add(nof_dispatches, std::memory_order_relaxed);
  uint64_t prev = s.batch_max_run.load(std::memory_order_relaxed);
  while (max_run > prev && !s.batch_max_run.compare_exchange_weak(prev, max_run, std::memory_order_relaxed)) {
  }
}
#else
static void demod_stats_commit() {}
static void demod_stats_wait() {}
static void demod_stats_batch(uint64_t, uint64_t, uint64_t) {}
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

/// A pointer and the byte offset a zero-copy wrap resolved to.
struct wrapped_buffer {
  id<MTLBuffer> buffer = nil;
  NSUInteger    offset = 0;
};

// Must match demod_params in ocudu_demod.metal.
struct demod_params_t {
  uint32_t nof_symbols; // OFDM symbols covered by the dispatch
  uint32_t nof_re;      // modulation symbols of one OFDM symbol
  uint32_t mod;
  uint32_t sym_stride; // float2 elements between two OFDM symbols of symbols[]
  uint32_t nv_stride;  // floats between two OFDM symbols of noise_var[]
  uint32_t llr_stride; // bytes between two OFDM symbols of llrs[]
};

/// Bits per modulation symbol of a kernel modulation id (0 = QPSK, 1 = 16QAM, 2 = 64QAM, 3 = 256QAM).
unsigned bits_per_symbol_of(unsigned mod)
{
  static constexpr unsigned bits[] = {2, 4, 6, 8};
  return (mod < (sizeof(bits) / sizeof(bits[0]))) ? bits[mod] : 0;
}

/// Parameters of a dispatch that covers ONE OFDM symbol whose arrays are packed: the layout every
/// caller that is not the deferred chain has (see enqueue()). \p nof_symbols is the number of
/// modulation symbols of that symbol - the batch dimension of the grid is 1, and the packed strides
/// (1 element, 1 variance and one LLR run of \c nof_symbols * bits) describe it.
demod_params_t packed_params(unsigned nof_symbols, unsigned mod)
{
  return demod_params_t{1, nof_symbols, mod, 1, 1, nof_symbols * bits_per_symbol_of(mod)};
}

/// Encodes one dispatch of \p params over the wrapped buffers.
void encode_demod(id<MTLComputeCommandEncoder> enc,
                  const wrapped_buffer&        b_sym,
                  const wrapped_buffer&        b_nv,
                  const wrapped_buffer&        b_llrs,
                  const demod_params_t&        params)
{
  [enc setBuffer:b_sym.buffer offset:b_sym.offset atIndex:0];
  [enc setBuffer:b_nv.buffer offset:b_nv.offset atIndex:1];
  [enc setBuffer:b_llrs.buffer offset:b_llrs.offset atIndex:2];
  [enc setBytes:&params length:sizeof(params) atIndex:3];
  [enc dispatchThreads:MTLSizeMake(params.nof_re, params.nof_symbols, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
}

/// Bytes the dispatch reads from / writes to each array, from its first OFDM symbol to the last.
size_t symbols_span_bytes(const demod_params_t& p)
{
  return (static_cast<size_t>(p.nof_symbols - 1) * p.sym_stride + p.nof_re) * 2 * sizeof(float);
}

size_t noise_span_bytes(const demod_params_t& p)
{
  return (static_cast<size_t>(p.nof_symbols - 1) * p.nv_stride + p.nof_re) * sizeof(float);
}

/// Bytes the dispatch writes to the LLR array, from its first OFDM symbol to the last.
///
/// \c llr_stride is already a byte count - the kernel reaches the symbol through
/// \c llrs_base + y * llr_stride on a \c char* (see ocudu_demod.metal) - so only the run of bits of
/// the last symbol is scaled by the modulation order. Scaling the stride as well (as this did until
/// S-7f-5x) overstates the span by the modulation order (2x to 8x): the request then exceeded the
/// buffer's allocation by that factor and the wrap cache re-mapped the buffer on every run whose
/// geometry changed, which is the "one replace per hop" the [metal_stats] wrap accounting reported.
size_t llr_span_bytes(const demod_params_t& p)
{
  return (static_cast<size_t>(p.nof_symbols - 1) * p.llr_stride) +
         (static_cast<size_t>(p.nof_re) * bits_per_symbol_of(p.mod));
}

/// \brief Bytes from \p ptr to the end of the allocation that contains it.
///
/// Returns SIZE_MAX when the process-wide registry does not describe the pointer, in which case the
/// caller has no bound to apply and keeps its previous behaviour (see wrap_length).
size_t allocation_bytes_left(const void* ptr)
{
  void*  base = nullptr;
  size_t size = 0;
  if (!compat::describe_aligned_allocation(ptr, &base, &size)) {
    return std::numeric_limits<size_t>::max();
  }
  const size_t offset = static_cast<size_t>(static_cast<const char*>(ptr) - static_cast<const char*>(base));
  return (size > offset) ? (size - offset) : 0;
}

/// \brief Length the no-copy mapping of \p ptr must cover for a run whose arrays reach \p span bytes.
///
/// The length a buffer is wrapped with must not follow the geometry of one run. The buffers of the
/// chained stages are reused across runs of different sizes, and the shared cache only hands a
/// mapping back when it covers the request, so a request that grows with the geometry re-maps the
/// buffer - a new MTLBuffer object over the same memory, one newBufferWithBytesNoCopy on the host's
/// hottest cell, and the per-object dependency tracking that reusing one mapping gives (see
/// shared_queue::wrap_no_copy and pilots_stage::buf_bytes, which exists for this very reason).
/// The allocation the pointer belongs to is therefore the right length: the whole buffer, not the
/// run, exactly like the estimator's pilot staging.
///
/// The run span is the fallback for a pointer the process-wide registry does not describe (a buffer
/// an engine allocated for itself), and a span that does not fit in its own allocation is a caller
/// defect: it is named once instead of being mapped past the end of the allocation, which is what
/// an overstated span does silently - until the pointer is one the registry does not know and the
/// mapping then covers memory the buffer does not own.
size_t wrap_length(const void* ptr, size_t span)
{
  void*       base = nullptr;
  size_t      size = 0;
  if (!compat::describe_aligned_allocation(ptr, &base, &size)) {
    return span;
  }
  const size_t offset    = static_cast<size_t>(static_cast<const char*>(ptr) - static_cast<const char*>(base));
  const size_t remaining = (size > offset) ? (size - offset) : 0;
  if (span > remaining) {
    static std::atomic<bool> warned{false};
    bool                     expected = false;
    if (warned.compare_exchange_strong(expected, true)) {
      ocudulog::fetch_basic_logger("PHY").error(
          "Metal demapper: a run reaches {} bytes but only {} bytes are left in the allocation at {}; "
          "the span of one of its arrays is overstated and the mapping overshoots the buffer",
          span,
          remaining,
          ptr);
    }
    return span;
  }
  return remaining;
}

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

wrapped_buffer wrap_buffer(demod_engine_impl* engine, const void* ptr, size_t span, size_t required_alignment = 1)
{
  // Every wrap of this engine goes through here, so the "length is the buffer, not the run" rule is
  // applied once, at the one place a caller cannot forget it (see wrap_length).
  const size_t length = wrap_length(ptr, span);
  // Same shared cache as the other engines: the demapper reads the symbols that the equalizer wrote
  // through the very same buffer object, so Metal tracks the dependency (see
  // shared_queue::wrap_no_copy).
  size_t        offset = 0;
  id<MTLBuffer> buf    = metal::shared_queue::wrap_no_copy(metal::shared_queue::device(), ptr, length, &offset);
  if ((buf != nil) && (required_alignment > 1) && ((offset % required_alignment) != 0)) {
    // The slice is inside a page-aligned allocation (so zero-copy is possible in principle) but its byte
    // offset is not a multiple of the element size of the kernel argument it is bound to: binding it would
    // be an alignment violation the CPU path never has - the GPU's own requirement, checked here instead of
    // assumed. It is counted (the "zero-copy wraps" contract check reads the counter) and staged instead.
    metal::shared_queue::notify_wrap_misaligned();
    static std::atomic<bool> misaligned_logged{false};
    bool                     expected = false;
    if (misaligned_logged.compare_exchange_strong(expected, true)) {
      ocudulog::fetch_basic_logger("PHY").warning(
          "Metal demapper: a slice starts at offset {} of its allocation, which is not a multiple of {} bytes; "
          "staging a copy instead of binding it",
          offset,
          required_alignment);
    }
    buf = nil;
  }
  if (buf != nil) {
    return wrapped_buffer{buf, static_cast<NSUInteger>(offset)};
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
  return wrapped_buffer{
      [metal::shared_queue::device() newBufferWithBytes:ptr length:length options:MTLResourceStorageModeShared], 0};
}

/// One OFDM symbol the deferred path accumulated instead of dispatching it (see
/// demod_metal_engine::enqueue_burst_deferred).
struct demod_pending_t {
  const void* symbols   = nullptr;
  const void* noise_var = nullptr;
  void*       llrs      = nullptr;
  unsigned    nof_re    = 0; // modulation symbols of this OFDM symbol
  unsigned    mod       = 0;
};

/// Per-thread accumulation of the deferred burst, one list per engine: the burst itself is thread
/// local and one thread can run several demodulators over its lifetime, so a list that outlived its
/// engine would be handed to the next engine's flush hook - which would then find its own (empty)
/// list and the group would never reach the GPU.
struct demod_flush_state_t {
  std::unordered_map<void*, std::vector<demod_pending_t>> pending;
};

demod_flush_state_t& demod_flush_state()
{
  static thread_local demod_flush_state_t s;
  return s;
}

std::vector<demod_pending_t>& demod_pending(void* engine)
{
  return demod_flush_state().pending[engine];
}

/// \brief Encodes the accumulated symbols of one group into the open burst.
///
/// A run of OFDM symbols sharing the modulation, the element count and the per-symbol array strides
/// becomes ONE dispatch: the kernel walks a (modulation symbols) x (OFDM symbols) grid, so a whole
/// run costs one dispatch instead of one per symbol - the dispatch itself (about 10us on this
/// hardware) dwarfs the kernel work of one 25 PRB symbol (a couple of microseconds). Nothing is
/// staged: the strides are exactly what the demodulator's page-aligned group buffers have.
id<MTLComputePipelineState> demod_flush_hook(void* context, id<MTLComputeCommandEncoder> enc)
{
  demod_engine_impl* engine = static_cast<demod_engine_impl*>(context);
  if ((enc == nil) || (engine == nullptr)) {
    return nil;
  }
  std::vector<demod_pending_t>& pending = demod_pending(context);
  if (pending.empty()) {
    return nil;
  }

  unsigned first      = 0;
  unsigned nof_disp   = 0;
  unsigned max_run    = 0;
  while (first != pending.size()) {
    const unsigned mod  = pending[first].mod;
    const unsigned bps  = bits_per_symbol_of(mod);
    const unsigned nof_re = pending[first].nof_re;
    if ((bps == 0) || (nof_re == 0)) {
      // Ill-formed entry: drop it rather than dispatching with a zero-sized grid.
      pending.clear();
      return nil;
    }

    // Extend the run while the next symbol keeps the geometry AND continues the same strides: one
    // grid covers the whole run, so every symbol must be reachable from the first one by a constant
    // step in each array. A symbol separated by a different gap simply starts a new run.
    //
    // A constant step is necessary but NOT sufficient: the whole run is bound to the kernel through
    // ONE mapping, and that mapping covers the allocation of the FIRST symbol and nothing else (see
    // wrap_length). Symbols that happen to sit in different allocations at a uniform distance would
    // satisfy the stride test and then be read - and written - past the end of the buffer object
    // that backs them, which the GPU does silently. This is not hypothetical: the deferred chain
    // stages every array it is given (the caller's buffers are not page aligned), and the staged
    // buffers of three submits are three allocations, ~80% of the runs of the offline gate landed on
    // a uniform distance and produced wrong LLRs. The run therefore has to stay inside the
    // allocation of its first symbol in all three arrays.
    const size_t sym_left = allocation_bytes_left(pending[first].symbols);
    const size_t nv_left  = allocation_bytes_left(pending[first].noise_var);
    const size_t llr_left = allocation_bytes_left(pending[first].llrs);
    size_t   sym_stride = 0;
    size_t   nv_stride  = 0;
    size_t   llr_stride = 0;
    unsigned n_sym      = 1;
    while (first + n_sym != pending.size()) {
      const demod_pending_t& prev = pending[first + n_sym - 1];
      const demod_pending_t& next = pending[first + n_sym];
      if ((next.mod != mod) || (next.nof_re != nof_re)) {
        break;
      }
      const ptrdiff_t d_sym = static_cast<const char*>(next.symbols) - static_cast<const char*>(prev.symbols);
      const ptrdiff_t d_nv  = static_cast<const char*>(next.noise_var) - static_cast<const char*>(prev.noise_var);
      const ptrdiff_t d_llr = static_cast<const char*>(next.llrs) - static_cast<const char*>(prev.llrs);
      // The strides are element counts of the arrays the kernel indexes, so a gap that is not a
      // whole number of elements cannot be expressed: it ends the run.
      if ((d_sym <= 0) || (d_nv <= 0) || (d_llr <= 0) || ((d_sym % static_cast<ptrdiff_t>(2 * sizeof(float))) != 0) ||
          ((d_nv % static_cast<ptrdiff_t>(sizeof(float))) != 0)) {
        break;
      }
      const size_t s_sym = static_cast<size_t>(d_sym) / (2 * sizeof(float));
      const size_t s_nv  = static_cast<size_t>(d_nv) / sizeof(float);
      const size_t s_llr = static_cast<size_t>(d_llr);
      if (n_sym > 1) {
        if ((s_sym != sym_stride) || (s_nv != nv_stride) || (s_llr != llr_stride)) {
          break;
        }
      } else {
        sym_stride = s_sym;
        nv_stride  = s_nv;
        llr_stride = s_llr;
      }
      // Span of the run that one more symbol would make, against the allocation of the first one.
      const size_t run_sym = (static_cast<size_t>(n_sym) * s_sym * 2 * sizeof(float)) + (nof_re * 2 * sizeof(float));
      const size_t run_nv  = (static_cast<size_t>(n_sym) * s_nv * sizeof(float)) + (nof_re * sizeof(float));
      const size_t run_llr = (static_cast<size_t>(n_sym) * s_llr) + (static_cast<size_t>(nof_re) * bps);
      if ((run_sym > sym_left) || (run_nv > nv_left) || (run_llr > llr_left)) {
        break;
      }
      ++n_sym;
    }

    const demod_params_t params{static_cast<uint32_t>(n_sym),
                                static_cast<uint32_t>(nof_re),
                                static_cast<uint32_t>(mod),
                                static_cast<uint32_t>(sym_stride),
                                static_cast<uint32_t>(nv_stride),
                                static_cast<uint32_t>(llr_stride)};

    engine->last_call_no_copy = true;
    wrapped_buffer b_sym = wrap_buffer(engine, pending[first].symbols, symbols_span_bytes(params), 8 /* device const float2* symbols */);
    wrapped_buffer b_nv  = wrap_buffer(engine, pending[first].noise_var, noise_span_bytes(params), alignof(float) /* device const float* noise_var */);
    wrapped_buffer b_llrs = wrap_buffer(engine, pending[first].llrs, llr_span_bytes(params), alignof(char) /* device char* llrs_base */);
    if ((b_sym.buffer == nil) || (b_nv.buffer == nil) || (b_llrs.buffer == nil)) {
      ocudulog::fetch_basic_logger("PHY").error("Metal demapper: no-copy wrap failed for a batched group");
      pending.clear();
      return nil;
    }
    encode_demod(enc, b_sym, b_nv, b_llrs, params);
    metal::shared_burst::count_dispatch(metal::shared_burst::stage::demapper);
    ++nof_disp;
    max_run = std::max(max_run, n_sym);
    first += n_sym;
  }
  demod_stats_batch(pending.size(), nof_disp, max_run);
  pending.clear();
  return demod_resources().pipeline;
}

/// Hands over and drops what this engine accumulated (its caller abandoned the group).
void demod_pending_release(void* engine)
{
  (void)metal::shared_burst::flush_pending();
  demod_pending(engine).clear();
  if (metal::shared_burst::flush_hook_context() == engine) {
    metal::shared_burst::set_flush_hook(nullptr, nullptr);
  }
}

} // namespace

demod_metal_engine::~demod_metal_engine()
{
  demod_engine_impl* engine = static_cast<demod_engine_impl*>(impl);
  if (engine != nullptr) {
    demod_pending_release(engine);
  }
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
  const demod_params_t params = packed_params(nof_symbols, mod);
  // wrap_buffer() clears this flag when a no-copy wrap falls back to a copy. Reset it before the
  // wraps (not after, where it would overwrite the outcome) so the diagnostic reports the truth.
  engine->last_call_no_copy = true;
  wrapped_buffer b_sym = wrap_buffer(engine, symbols, symbols_span_bytes(params), 8 /* device const float2* symbols */);
  wrapped_buffer b_nv = wrap_buffer(engine, noise_var, noise_span_bytes(params), alignof(float) /* device const float* noise_var */);
  wrapped_buffer b_llrs = wrap_buffer(engine, llrs, llr_span_bytes(params), alignof(char) /* device char* llrs_base */);
  if (b_sym.buffer == nil || b_nv.buffer == nil || b_llrs.buffer == nil) {
    return false;
  }
  encode_demod(engine->batch_enc, b_sym, b_nv, b_llrs, params);
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
  const demod_params_t params = packed_params(nof_symbols, mod);

  engine->last_call_no_copy = true;
  wrapped_buffer b_sym = wrap_buffer(engine, symbols, symbols_span_bytes(params), 8 /* device const float2* symbols */);
  wrapped_buffer b_nv = wrap_buffer(engine, noise_var, noise_span_bytes(params), alignof(float) /* device const float* noise_var */);
  wrapped_buffer b_llrs = wrap_buffer(engine, llrs, llr_span_bytes(params), alignof(char) /* device char* llrs_base */);
  if (b_sym.buffer == nil || b_nv.buffer == nil || b_llrs.buffer == nil) {
    return false;
  }
  encode_demod(enc, b_sym, b_nv, b_llrs, params);
  metal::shared_burst::count_dispatch(metal::shared_burst::stage::demapper);
  return true;
}

bool demod_metal_engine::enqueue_burst_deferred(const void* symbols,
                                                const void* noise_var,
                                                void*       llrs,
                                                unsigned    nof_re,
                                                unsigned    mod)
{
  demod_engine_impl* engine = static_cast<demod_engine_impl*>(impl);
  if ((engine == nullptr) || (symbols == nullptr) || (noise_var == nullptr) || (llrs == nullptr) || (nof_re == 0)) {
    return false;
  }
  // The FIRST submission of a group opens the burst through encoder(): that hands over the previous
  // stage's accumulated dispatches (the equalization) with a live encoder, switches the pipeline -
  // which inserts the memory barrier that orders this stage after them - and leaves the burst's
  // stage set to the demapping one, so the symbols accumulated below are encoded behind that
  // barrier. Later submissions of the same engine only accumulate: going through encoder() again
  // would flush this engine's hook, i.e. one dispatch per symbol, which is what the batching is here
  // to avoid.
  if (metal::shared_burst::flush_hook_context() != engine) {
    if (metal::shared_burst::encoder(demod_resources().pipeline) == nil) {
      return false;
    }
  }
  demod_pending(engine).push_back({symbols, noise_var, llrs, nof_re, mod});
  metal::shared_burst::set_flush_hook(engine, &demod_flush_hook);
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
  // The GPU-time probe must be armed before commit (Metal asserts otherwise).
  metal::shared_queue::arm_gpu_time(cmd_buf, metal::shared_queue::queue_kind::back_end, "demapper");
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
