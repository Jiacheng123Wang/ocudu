// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu_equalizer_metal_engine.h"
#include "ocudu_metal_burst.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "ocudu_metal_queue.h"

#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/phy/phy_pipeline_crossings.h"
#include "ocudu/support/macos_compat.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

/// \brief Declares this module as audited for the crossing count.
///
/// The equalizer's two inputs are device-fed on the lane route - ch_re_source()/ch_est_source() are
/// incremented AT the branch points, and on air they read device=10098 host=0 and device=10098
/// staged=0 - and its host -> device writes are the two table builders, both counted. The zero-copy
/// fallback does not run: the shared wrap accounting reports 0 failures and 0 misaligned on air.
struct equalizer_crossing_declaration {
  equalizer_crossing_declaration() { ocudu::phy_pipeline_crossings::declare_reporter("equalizer"); }
} equalizer_crossing_declaration_instance;

} // namespace
#include <vector>

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

/// Batch diagnostics of the deferred burst (see equalizer_metal_engine::batch_diag). Kept in every
/// build: the counters cost a few relaxed increments per flush and are the only way to tell a group
/// that fell back to one dispatch per symbol from one that was batched.
struct eq_batch_diag_t {
  std::atomic<uint64_t> flushes{0};
  std::atomic<uint64_t> symbols{0};
  std::atomic<uint64_t> runs{0};
  std::atomic<uint64_t> batched_runs{0};
  std::atomic<unsigned> max_run{0};
  std::atomic<const char*> first_break{nullptr};
};

eq_batch_diag_t& eq_batch_diag()
{
  // Deliberately leaked: this is reported while other static destructors may already have run.
  static eq_batch_diag_t* s = new eq_batch_diag_t();
  return *s;
}

/// Records the first predicate that stopped a run from extending.
void eq_batch_note_break(const char* reason)
{
  const char* expected = nullptr;
  eq_batch_diag().first_break.compare_exchange_strong(expected, reason, std::memory_order_relaxed);
}

/// Raises the recorded longest run to \p run.
void eq_batch_note_run(unsigned run)
{
  unsigned prev = eq_batch_diag().max_run.load(std::memory_order_relaxed);
  while ((run > prev) &&
         !eq_batch_diag().max_run.compare_exchange_weak(prev, run, std::memory_order_relaxed)) {
  }
}

#if defined(OCUDU_METAL_STATS)
const bool eq_batch_diag_registered = []() {
  std::atexit([]() {
    const eq_batch_diag_t& d = eq_batch_diag();
    const char*            brk = d.first_break.load(std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[metal_stats] eq_batch flushes=%llu symbols=%llu runs=%llu batched=%llu max_run=%u first_break=%s\n",
                 static_cast<unsigned long long>(d.flushes.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(d.symbols.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(d.runs.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(d.batched_runs.load(std::memory_order_relaxed)),
                 d.max_run.load(std::memory_order_relaxed),
                 (brk == nullptr) ? "none" : brk);
  });
  return true;
}();
#endif // OCUDU_METAL_STATS

struct eq_resources_t {
  id<MTLDevice>               device         = nil;
  id<MTLCommandQueue>         queue          = nil;
  id<MTLComputePipelineState> pipeline       = nil;
  id<MTLComputePipelineState> pipeline_batch = nil;
  /// Reads the equalizer's received symbols off the device resource grid (see gather_binding).
  id<MTLComputePipelineState> pipeline_gather = nil;
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

/// Bytes a gather dispatch may read from the device grid: the whole storage of the view, since the
/// entries it names are spread over every port, symbol and subcarrier of the allocation.
static size_t grid_view_bytes(const resource_grid_device_view& grid)
{
  const size_t elements = static_cast<size_t>(grid.port_stride) * grid.nof_ports;
  return elements * sizeof(cbf16_t);
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

// Must match gather_params / gather_tap / gather_entry in ocudu_equalizer.metal.
struct gather_params_t {
  uint64_t grid_base;        // byte offset of grid element 0 of port 0, symbol 0
  uint32_t nof_symbols;      // OFDM symbols of the run
  uint32_t nof_ports;        // receive ports to gather
  uint32_t nof_dest;         // cbf16_t per port run (the dispatch's nof_re)
  uint32_t grid_subc_stride; // elements between two consecutive subcarriers
  uint32_t grid_symb_stride; // elements between two consecutive OFDM symbols
  uint32_t grid_port_stride; // elements between two consecutive ports
  uint32_t first_symbol;     // first symbol of the run within the hop's tap table
};

struct gather_tap_t {
  uint32_t symbol;
  uint32_t offset;
  uint32_t nof_re;
};

struct gather_entry_t {
  uint32_t subc;
  uint32_t dest;
};

/// \brief The plan tables of one gather dispatch, in one allocation.
///
/// The two tables a gather dispatch reads are built from the caller's plan at encode time: one tap
/// per OFDM symbol of the run (its grid symbol, where its entries start, how many there are) and the
/// entries themselves, copied out of the plan. The blob is handed to the flush state, which keeps it
/// alive until the command buffer that reads it completed - the same rule the staged inputs follow.
using gather_tables_t = std::vector<uint8_t>;

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

/// One start table per dispatch, freshly allocated.
///
/// The table is small and there is one per run, but it must survive until the command buffer that
/// reads it has executed - and slicing one shared buffer per dispatch with setBuffer:offset: proved
/// unreliable (a dispatch read its neighbour's slice). A dedicated buffer per dispatch removes the
/// offset from the picture entirely; the allocation is a few dozen bytes against a group that
/// already allocates its staging, so the cost is noise next to the dispatch it describes.
/// \brief Content-keyed cache for the two host-built tables the device reads.
///
/// Both are pure functions of the hop's plan and of the symbol layout, so both are the SAME BYTES
/// hop after hop for a given allocation. Building one is a Metal buffer allocation plus an upload, a
/// real host -> device crossing; caching removes it from the steady state, leaving one crossing per
/// DISTINCT table instead of one per dispatch and per hop.
///
/// Keyed by CONTENT, not by the caller's pointer: the plan object is rebuilt every hop, so its
/// address is not a key, but its bytes are. The hit path is a memcmp over host memory - it touches
/// no device buffer and waits on no GPU, which is why count_host_write() is deliberately NOT called
/// on a hit. A hit must leave the crossing count at zero, not "small".
///
/// \note Measuring this needs more than one hop: ul_chain_replay --repeat N is the ruler, because a
///       single-hop replay has a cold cache and cannot tell a cache from no cache at all. That is
///       exactly how the first attempt at this batch failed to show anything.
struct eq_table_cache {
  struct entry {
    std::vector<unsigned char> key;
    id<MTLBuffer>              buf = nil;
  };
  std::vector<entry> entries;
};

eq_table_cache& eq_table_cache_of()
{
  // Thread local: the engines are per-thread. Never destroyed (the contract report runs at exit).
  static thread_local eq_table_cache* c = new eq_table_cache();
  return *c;
}

id<MTLBuffer> eq_cached_table(id<MTLDevice> device, const void* data, size_t bytes)
{
  if ((device == nil) || (data == nullptr) || (bytes == 0)) {
    return nil;
  }
  eq_table_cache&      c = eq_table_cache_of();
  const unsigned char* p = static_cast<const unsigned char*>(data);
  for (const eq_table_cache::entry& e : c.entries) {
    if ((e.key.size() == bytes) && (std::memcmp(e.key.data(), p, bytes) == 0)) {
      return e.buf; // HIT: nothing was written, so nothing is counted.
    }
  }
  // MISS: the crossing - an allocation and an upload into memory the device reads.
  phy_pipeline_crossings::count_host_write_site("equalizer: table uploaded (cache miss)", bytes);
  id<MTLBuffer> buf = [device newBufferWithBytes:data
                                         length:static_cast<NSUInteger>(bytes)
                                        options:MTLResourceStorageModeShared];
  if (buf != nil) {
    eq_table_cache::entry e;
    e.key.assign(p, p + bytes);
    e.buf = buf;
    c.entries.push_back(e);
  }
  return buf;
}

static id<MTLBuffer> eq_make_h_starts(id<MTLDevice> device, const unsigned* starts, unsigned n_run)
{
  if ((device == nil) || (n_run == 0)) {
    return nil;
  }
  // Batch 0: built once per distinct table and reused. The cache keeps a DEDICATED buffer per table,
  // so the correctness fix this function exists for - a dispatch reading its neighbour's slice when
  // one shared buffer was sliced with setBuffer:offset: - is untouched.
  return eq_cached_table(device, starts, static_cast<size_t>(n_run) * sizeof(unsigned));
}

struct wrapped_buffer {
  id<MTLBuffer> buffer = nil;
  NSUInteger    offset = 0;
};

/// Hand the deferred dispatches \p engine accumulated in this thread over to the burst (defined
/// next to the flush state below; the engine destructor only needs the declaration).
void eq_pending_release(void* engine);

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

/// One dispatch's per-symbol estimate starts as its own Metal buffer (see eq_make_h_starts), or nil
/// when the table cannot be created.
static id<MTLBuffer> eq_bind_h_starts(const unsigned* starts, unsigned n_run)
{
  id<MTLBuffer> buf = eq_make_h_starts(metal::shared_queue::device(), starts, n_run);
  if (buf == nil) {
    ocudulog::fetch_basic_logger("PHY").error(
        "Metal equalizer: cannot allocate the per-symbol start table ({} symbols)", n_run);
  }
  return buf;
}

/// One-entry start table of a single-symbol dispatch.
static id<MTLBuffer> bind_one_h_start(unsigned start)
{
  const unsigned one[1] = {start};
  return eq_bind_h_starts(one, 1);
}

equalizer_metal_engine::~equalizer_metal_engine()
{
  eq_engine_impl* engine = static_cast<eq_engine_impl*>(impl);
  if (engine != nullptr) {
    // Hand the dispatches this engine still had accumulated in this thread over to the burst: every
    // path that reaches wait() flushes them, so a non-empty list means a caller that never waited,
    // and neither the list nor the flush hook may outlive the engine they belong to.
    eq_pending_release(engine);
  }
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
    // Reads the received symbols off the device resource grid, so the equalizer's y input never
    // crosses the host (see gather_binding).
    id<MTLFunction> fn_gather = [library newFunctionWithName:@"gather_ch_re"];
    if (fn_gather == nil) {
      ocudulog::fetch_basic_logger("PHY").error("Metal equalizer: kernel 'gather_ch_re' not found");
      return false;
    }
    res.pipeline_gather = [res.device newComputePipelineStateWithFunction:fn_gather error:&error];
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
  id<MTLBuffer> b_start = bind_one_h_start(h.offset);
  if (b_start == nil) {
    return false;
  }
  [enc setBuffer:b_start offset:0 atIndex:7];
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
  /// True when \c h points into the buffer the channel estimator produced the estimates in, i.e.
  /// when its contents are written by a GPU dispatch that may still be in flight. Such a binding is
  /// never read on the host by the batched encoding: the dispatch reads it, exactly like the
  /// per-symbol path does (see eq_flush_hook).
  bool        h_on_device = false;
  /// Where the received symbols of this symbol come from. Valid means the resource grid holds them
  /// and the gather reads them there, so the caller did NOT stage them (see gather_binding).
  equalizer_metal_engine::gather_binding gather;
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

/// Per-thread accumulation of the deferred burst (the burst itself is thread local), one list per
/// engine.
///
/// The equalizer submits one symbol per call, which used to encode one dispatch per symbol. The
/// dispatches are accumulated here instead and handed over through shared_burst's flush hook right
/// before the burst changes stage or commits, so a group of symbols whose geometry, noise path and
/// output strides match becomes ONE batched dispatch - without the caller having to collect
/// anything (the inputs are staged per symbol, exactly as before).
///
/// The lists are per engine because one thread can run several equalizers over its lifetime - the
/// concurrency test creates one per worker and switches between sequential demodulations - and a
/// list that survives its engine would be handed to the flush hook of the next one: the hook then
/// finds its own empty list, encodes nothing, and the group of the new engine never reaches the
/// GPU (the soft bits of every symbol but the first of the group stay zero).
struct eq_flush_state_t {
  std::unordered_map<void*, std::vector<eq_pending_t>> pending;
  /// Group staging handed over to a committed command buffer, recycled by the next flush (the
  /// kernels read it until that command buffer completes).
  std::vector<void*> inflight;
  /// Gather plan tables handed to a committed command buffer, kept alive the same way. They are
  /// Metal buffers, so they are owned (released) rather than freed.
  std::vector<id<MTLBuffer>> gather_tables;
  /// The hop's tables while the burst is being encoded (see eq_gather_tables): one build per hop,
  /// every dispatch of that hop reads them.
  gather_tables_t hop_tables;
  /// The plan they were built from, as an address. It must hold a POINTER: an unsigned truncated it
  /// on a 64-bit host and the key never matched the plan it was made from, so the cache never hit.
  ///
  /// \note This key is only valid WITHIN one hop (one flush): the demodulator allocates a fresh plan
  ///       object per hop, and the allocator hands the same address back for it, so an address match
  ///       across hops means "a different plan that happens to live where the old one lived". The
  ///       cache is invalidated when the flush is recycled (see eq_flush_recycle).
  uintptr_t hop_tables_plan  = 0;
  bool      hop_tables_valid = false;
};
static eq_flush_state_t& eq_flush_state()
{
  static thread_local eq_flush_state_t s;
  return s;
}

/// Accumulated dispatches of \p engine in this thread.
static std::vector<eq_pending_t>& eq_pending(void* engine)
{
  return eq_flush_state().pending[engine];
}

namespace {
void eq_pending_release(void* engine)
{
  // The work this engine accumulated must not outlive it: the entries point at the caller's input
  // buffers, and their addresses are reusable, so a later engine that happens to be allocated at
  // the same address would inherit them and encode a group of dangling dispatches. Handing it over
  // keeps the burst's dispatches; dropping it would leave the caller's outputs at their old values.
  metal::shared_burst::flush_pending();
  eq_pending(engine).clear();
  if (metal::shared_burst::flush_hook_context() == engine) {
    metal::shared_burst::set_flush_hook(nullptr, nullptr);
  }
}
/// Recycles the group staging of the previous flush (its command buffer was waited for).
static void eq_flush_recycle()
{
  eq_flush_state_t& st = eq_flush_state();
  for (void* p : st.inflight) {
    compat::aligned_free(p);
  }
  st.inflight.clear();
  st.gather_tables.clear();
  // The cached gather tables die with the hop that built them: they are read by command buffers this
  // flush has already handed over, and their identity key (the plan's address) is REUSABLE - the
  // demodulator builds a new plan object per hop, and the allocator hands back the same address for
  // it. Keeping the cache across hops therefore served the previous hop's tables to the new hop,
  // which is worse than a missing cache: the entries name the wrong resource elements.
  st.hop_tables_valid = false;
}

/// Encodes every accumulated symbol into the open burst: one batched dispatch per run of symbols
/// that share geometry, noise path, scaling and output strides, and the per-symbol kernel for the
/// runs that do not. Returns the pipeline the dispatches were encoded with.
/// \brief Encodes one batched group dispatch onto \p enc and returns the pipeline it used.
///
/// Both routes into the batched kernel go through this: the deferred flush (which owns the run's
/// staging and the estimator's device slice) and the explicit enqueue_burst_batch_at() entry (which
/// is handed the caller's arrays). The binding sequence lives here exactly once - two copies is
/// what let the deferred route drift away from the entry the probe validates.
/// \param h_base_bytes Byte offset of the run's first estimate inside \p b_h.
static id<MTLComputePipelineState> eq_encode_batch_dispatch(id<MTLComputeCommandEncoder> enc,
                                                           const wrapped_buffer&         b_h,
                                                           const wrapped_buffer&         b_y,
                                                           const wrapped_buffer&         b_eq,
                                                           const wrapped_buffer&         b_nv,
                                                           const wrapped_buffer&         b_s,
                                                           id<MTLBuffer>                 b_starts,
                                                           const equalize_params_t&      params,
                                                           const eq_strides_t&           strides,
                                                           size_t                        h_base_bytes,
                                                           unsigned                      nof_re,
                                                           unsigned                      n_run)
{
  if ((enc == nil) || (b_h.buffer == nil) || (b_y.buffer == nil) || (b_eq.buffer == nil) ||
      (b_nv.buffer == nil) || (b_s.buffer == nil) || (b_starts == nil)) {
    return nil;
  }
  id<MTLComputePipelineState> pipeline = (n_run > 1) ? eq_resources().pipeline_batch : eq_resources().pipeline;
  [enc setComputePipelineState:pipeline];
  [enc setBuffer:b_h.buffer offset:(b_h.offset + h_base_bytes) atIndex:0];
  [enc setBuffer:b_y.buffer offset:b_y.offset atIndex:1];
  [enc setBuffer:b_eq.buffer offset:b_eq.offset atIndex:2];
  [enc setBuffer:b_nv.buffer offset:b_nv.offset atIndex:3];
  [enc setBytes:&params length:sizeof(params) atIndex:4];
  [enc setBuffer:b_s.buffer offset:b_s.offset atIndex:5];
  if (n_run > 1) {
    [enc setBytes:&strides length:sizeof(strides) atIndex:6];
    [enc setBuffer:b_starts offset:0 atIndex:7];
  } else {
    [enc setBuffer:b_starts offset:0 atIndex:7];
  }
  [enc dispatchThreads:MTLSizeMake(nof_re, n_run, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
  metal::shared_burst::count_dispatch(metal::shared_burst::stage::equalizer);
  return pipeline;
}

/// \brief Builds the tap and entry tables of a whole hop.
///
/// The tables describe the hop's ALLOCATION, not the symbols one dispatch happens to carry: the
/// entries of a symbol and their positions in the table do not depend on which symbols travel
/// together. So one table serves every dispatch of the hop - the kernel enters it at the run's first
/// symbol (see gather_params::first_symbol) - and it is built once per hop, not once per dispatch.
/// That matters: a rebuild costs a copy of the entry table (tens of KB for a wide allocation) plus
/// two Metal buffer allocations, and the pipeline reaches four dispatches per slot.
///
/// \param[in] plan The hop's plan, owned by the caller.
/// \return The tables, or an empty blob when the plan describes no symbols.
static gather_tables_t eq_build_gather_tables(const ch_gather_desc& plan)
{
  gather_tables_t blob;
  if (!plan.is_valid()) {
    return blob;
  }
  // One tap per symbol of the hop: the run's symbols are a window into it, so the table does not
  // depend on the window and stays valid for every dispatch of the hop.
  const size_t taps_bytes    = static_cast<size_t>(plan.nof_symbols) * sizeof(gather_tap_t);
  const size_t entries_bytes = plan.size() * sizeof(gather_entry_t);
  blob.resize(taps_bytes + entries_bytes);
  auto* taps    = reinterpret_cast<gather_tap_t*>(blob.data());
  auto* entries = reinterpret_cast<gather_entry_t*>(blob.data() + taps_bytes);

  for (unsigned k = 0; k != plan.nof_symbols; ++k) {
    const ch_gather_symbol& run = plan.symbols[k];
    taps[k] = gather_tap_t{run.symbol, run.entry_base, run.nof_entries};
  }
  for (unsigned k = 0; k != plan.size(); ++k) {
    entries[k] = gather_entry_t{plan.entries[k].subc, plan.entries[k].dest};
  }
  return blob;
}

/// \brief One gather table as its own Metal buffer.
///
/// The taps and the entries live in one host allocation, but each is bound to its own MTLBuffer:
/// slicing a shared buffer with setBuffer:offset: is what a dispatch read its neighbour's slice
/// through once already (see eq_make_h_starts), and a mis-resolved offset here makes the kernel
/// read a plan that is not the one it was handed.
static id<MTLBuffer> eq_make_gather_table(const void* data, size_t bytes)
{
  if ((data == nullptr) || (bytes == 0)) {
    return nil;
  }
  // Batch 0: the tables "describe the allocation" and do not depend on which symbols a dispatch
  // carries, so the same bytes come back hop after hop for a given plan. Cached instead of rebuilt.
  return eq_cached_table(metal::shared_queue::device(), data, bytes);
}

/// \brief The hop's gather tables as Metal buffers, built once and reused by every dispatch of the
/// hop (see eq_build_gather_tables).
///
/// The tables a gather dispatch reads do NOT depend on which symbols the dispatch carries: they
/// describe the allocation, and the kernel enters them at the run's first symbol. So they are built
/// and uploaded once per hop and shared by the (up to four) dispatches of that hop - rebuilding
/// them per dispatch is a copy of the entry table plus two Metal buffer allocations EACH, which is
/// what made the device gather cost more on the host than the memcpy it replaces.
///
/// \param[in] plan The hop's plan.
/// \return False when the tables could not be built or uploaded.
static bool eq_gather_tables(const ch_gather_desc& plan)
{
  eq_flush_state_t& st = eq_flush_state();
  if (st.hop_tables_valid && (st.hop_tables_plan == reinterpret_cast<uintptr_t>(&plan))) {
    return true;
  }
  st.hop_tables       = eq_build_gather_tables(plan);
  st.hop_tables_plan  = reinterpret_cast<uintptr_t>(&plan);
  st.hop_tables_valid = !st.hop_tables.empty();
  if (!st.hop_tables_valid) {
    return false;
  }
  // The buffers are handed to a command buffer, so they are kept until it completed - the same rule
  // the staging follows (see eq_flush_recycle).
  const size_t taps_bytes = static_cast<size_t>(plan.nof_symbols) * sizeof(gather_tap_t);
  id<MTLBuffer> b_taps    = eq_make_gather_table(st.hop_tables.data(), taps_bytes);
  id<MTLBuffer> b_entries = eq_make_gather_table(st.hop_tables.data() + taps_bytes, st.hop_tables.size() - taps_bytes);
  if ((b_taps == nil) || (b_entries == nil)) {
    st.hop_tables_valid = false;
    return false;
  }
  st.gather_tables.push_back(b_taps);
  st.gather_tables.push_back(b_entries);
  return true;
}

/// The hop's tap buffer, or nil when eq_gather_tables() has not built one.
static id<MTLBuffer> eq_gather_taps()
{
  const eq_flush_state_t& st = eq_flush_state();
  return (st.gather_tables.size() >= 2) ? st.gather_tables[st.gather_tables.size() - 2] : nil;
}

/// The hop's entry table buffer, or nil when eq_gather_tables() has not built one.
static id<MTLBuffer> eq_gather_entries()
{
  const eq_flush_state_t& st = eq_flush_state();
  return st.gather_tables.empty() ? nil : st.gather_tables.back();
}

/// \brief Dispatches the gather of the run's received symbols off the device grid.
///
/// One thread per (resource element, OFDM symbol of the run). The device work is a plain copy, and
/// it is encoded in the same command buffer right before the equalization that consumes it: the
/// equalization reads y afterwards, so the barrier the burst inserts between pipelines orders the
/// two (the gather is its own pipeline, so the change is one).
/// \return False when the dispatch could not be encoded; the caller then keeps the host-gathered y.
static bool eq_encode_gather_dispatch(id<MTLComputeCommandEncoder> enc,
                                      const wrapped_buffer&          b_grid,
                                      const wrapped_buffer&          b_y,
                                      const ch_gather_desc&          plan,
                                      unsigned                       first_symbol,
                                      unsigned                       n_sym,
                                      unsigned                       nof_ports,
                                      unsigned                       nof_re,
                                      const gather_tables_t&         tables,
                                      id<MTLBuffer>                  b_taps,
                                      id<MTLBuffer>                  b_entries)
{
  if ((enc == nil) || (b_grid.buffer == nil) || (b_y.buffer == nil) || (b_taps == nil) ||
      (b_entries == nil) || (tables.empty()) || (n_sym == 0)) {
    return false;
  }
  const gather_params_t p{b_grid.offset,
                          n_sym,
                          nof_ports,
                          nof_re,
                          plan.grid.subc_stride,
                          plan.grid.symb_stride,
                          plan.grid.port_stride,
                          first_symbol};
  [enc setComputePipelineState:eq_resources().pipeline_gather];
  [enc setBuffer:b_grid.buffer offset:0 atIndex:0];
  [enc setBuffer:b_y.buffer offset:b_y.offset atIndex:1];
  [enc setBytes:&p length:sizeof(p) atIndex:2];
  [enc setBuffer:b_entries offset:0 atIndex:3];
  [enc setBuffer:b_taps offset:0 atIndex:4];
  [enc dispatchThreads:MTLSizeMake(nof_re, n_sym, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
  metal::shared_burst::count_dispatch(metal::shared_burst::stage::equalizer);
  return true;
}

static id<MTLComputePipelineState> eq_flush_hook(void* context, id<MTLComputeCommandEncoder> enc)
{
  eq_engine_impl* engine = static_cast<eq_engine_impl*>(context);
  if ((enc == nil) || (engine == nullptr)) {
    return nil;
  }
  // This engine's pending list in this thread (see eq_flush_state_t): the hook is installed with the
  // engine as its context, so a thread that runs several equalizers encodes the group of the one
  // that installed it.
  std::vector<eq_pending_t>& pending = eq_pending(context);
  if (pending.empty()) {
    return nil;
  }
  // The previous flush's command buffer has been waited for before a new group is accumulated.
  eq_flush_recycle();

  const unsigned n_sym = static_cast<unsigned>(pending.size());
  unsigned       first = 0;
  bool           any_batch = false;
  id<MTLComputePipelineState> used_pipeline = nil;
  eq_batch_diag_t& diag = eq_batch_diag();
  diag.flushes.fetch_add(1, std::memory_order_relaxed);
  diag.symbols.fetch_add(n_sym, std::memory_order_relaxed);
  while (first != n_sym) {
    const eq_pending_t& head = pending[first];
    // Extend the run while geometry, noise path, scalings, noise variances and output strides match.
    unsigned n_run = 1;
    while (first + n_run != n_sym) {
      const eq_pending_t& next = pending[first + n_run];
      const eq_pending_t& prev = pending[first + n_run - 1];
      const bool same_geom = (next.nof_re == head.nof_re) && (next.nof_ports == head.nof_ports) &&
                             (next.nof_layers == head.nof_layers) && (next.mmse == head.mmse) &&
                             (next.noise_var == head.noise_var) && (next.tx_scaling == head.tx_scaling) &&
                             (next.h_scaling == head.h_scaling);
      // The estimates of the run must be describable by ONE dispatch: the batched kernel reads them
      // all through one binding, stepping by a fixed number of elements per symbol. A run of device
      // slices is NOT copied to the host (see eq_flush_hook) - their producer may still be running,
      // and only the GPU read, ordered through the queue, sees its writes - so it must also share
      // one buffer and advance by exactly that step, which is what the estimator's offsets do.
      const unsigned h_step = (head.h.layer_stride != 0) ? head.h.layer_stride : head.nof_re;
      const unsigned h_want = prev.h.offset + h_step;
      const bool     same_h = (next.h.layer_stride == head.h.layer_stride) &&
                          (next.h_on_device == head.h_on_device) &&
                          (!next.h_on_device ||
                           ((next.h.buffer == head.h.buffer) && (next.h.offset == h_want)));
      const bool same_strides =
          (static_cast<const char*>(next.eq) - static_cast<const char*>(prev.eq)) ==
              (static_cast<const char*>(pending[first + 1].eq) - static_cast<const char*>(head.eq)) &&
          (static_cast<const char*>(next.nv) - static_cast<const char*>(prev.nv)) ==
              (static_cast<const char*>(pending[first + 1].nv) - static_cast<const char*>(head.nv)) &&
          (next.eq != nullptr) && (next.nv != nullptr);
      const bool same_sigma = (std::memcmp(next.sigma2, head.sigma2, head.nof_ports * sizeof(float)) == 0);
      // The received symbols of the run must come from the same place: either every symbol was
      // staged by the caller, or every one of them is read off the device grid through the same
      // plan, at the symbol the run's own symbol table describes (the tables are built for
      // head.gather.symbol + k, so the symbols have to be consecutive in the grid).
      const bool same_gather = next.gather.is_valid() == head.gather.is_valid() &&
                               (!head.gather.is_valid() ||
                                ((next.gather.desc == head.gather.desc) &&
                                 (next.gather.symbol == head.gather.symbol + n_run)));
      if (!same_geom || !same_h || !same_strides || !same_sigma || !same_gather) {
        eq_batch_note_break(!same_geom  ? "geometry"
                            : !same_h   ? "estimates"
                            : !same_strides ? "strides"
                            : !same_sigma   ? "sigma2"
                                            : "gather");
        break;
      }
      ++n_run;
    }
    diag.runs.fetch_add(1, std::memory_order_relaxed);
    if (n_run > 1) {
      diag.batched_runs.fetch_add(1, std::memory_order_relaxed);
    }
    eq_batch_note_run(n_run);

    // Group staging: h [symbol][port][layer][re], y [symbol][port][re], one sigma2 array per run.
    const size_t h_stride = static_cast<size_t>(head.nof_ports) * head.nof_layers * head.nof_re;
    const size_t y_stride = static_cast<size_t>(head.nof_ports) * head.nof_re;
    const unsigned eq_stride_elems =
        (n_run > 1) ? static_cast<unsigned>((static_cast<const char*>(pending[first + 1].eq) -
                                             static_cast<const char*>(head.eq)) /
                                            2 / sizeof(float))
                    : 0;
    const unsigned nv_stride_elems =
        (n_run > 1) ? static_cast<unsigned>((static_cast<const char*>(pending[first + 1].nv) -
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
    // A run whose estimates were produced on the device is read THERE, with the per-symbol offset
    // step the kernel applies through h_layer_stride (see the run predicate): copying it to the host
    // would read a buffer whose producing dispatch may not have completed - the host has no wait
    // that covers it - and the equalization would run on whatever was there.
    const bool h_device_run = pending[first].h_on_device;
    // A run whose received symbols are read off the device grid is gathered THERE too: the caller
    // planned the hop (ch_gather_desc), so the elements are copied out of the grid by a dispatch of
    // this same command buffer instead of by one memcpy per port per symbol here. The run predicate
    // made sure every symbol of the run shares the plan.
    const bool        gather_run = pending[first].gather.is_valid();
    const ch_gather_desc* gather_plan = gather_run ? pending[first].gather.desc : nullptr;
    // The run's entry point into the hop's tap table: only a gathered run has one.
    const unsigned first_symbol =
        gather_run ? (pending[first].gather.symbol - gather_plan->symbols[0].symbol) : 0u;
    if (gather_run && !eq_gather_tables(*gather_plan)) {
      compat::aligned_free(h_alloc);
      compat::aligned_free(y_alloc);
      compat::aligned_free(s_alloc);
      return nil;
    }
    for (unsigned k = 0; k != n_run; ++k) {
      if (!h_device_run) {
        std::memcpy(h_alloc + k * h_stride,
                    static_cast<const cbf16_t*>(pending[first + k].h.buffer) + pending[first + k].h.offset,
                    h_stride * sizeof(cbf16_t));
      }
      if (!gather_run) {
        std::memcpy(y_alloc + k * y_stride, pending[first + k].y, y_stride * sizeof(cbf16_t));
      }
    }
    equalizer_metal_engine::ch_est_binding h_run_binding =
        h_device_run ? pending[first].h : equalizer_metal_engine::ch_est_binding(h_alloc);
    equalize_params_t params = make_params(h_run_binding,
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

    // The estimates are wrapped where they live: the host staging of a staged run, or the buffer the
    // estimator produced them in for a device run (which the kernel then reads through the queue).
    //
    // The per-symbol starts come from the estimator, NOT from a stride: a symbol carrying DM-RS
    // holds fewer data REs, so its slice is shorter and the next one starts at an irregular
    // distance (72, 108, 72, ... elements). One stride cannot describe that, so the run carries the
    // starts in a table and h_bytes reaches through the LAST symbol's own start.
    const unsigned h_last  = (n_run > 1) ? pending[first + n_run - 1].h.offset : h_run_binding.offset;
    const size_t   h_bytes = ch_est_binding_bytes(h_run_binding, head.nof_ports, head.nof_layers, head.nof_re) +
                           static_cast<size_t>(h_last - h_run_binding.offset) * sizeof(cbf16_t);
    // A gather dispatch fills the run's y region, so the binding must reach the end of its last
    // symbol - whose length is its own nof_re, not the run stride (a symbol is padded to the
    // dispatch's geometry, and the gather skips the tail).
    const size_t y_bytes = (gather_run && (n_run > 1))
                               ? ((static_cast<size_t>(n_run) - 1) * y_stride +
                                  static_cast<size_t>(pending[first + n_run - 1].nof_re) * head.nof_ports) *
                                     sizeof(cbf16_t)
                               : y_stride * n_run * sizeof(cbf16_t);

    const size_t s_bytes  = static_cast<size_t>(head.nof_ports) * sizeof(float);
    const size_t eq_bytes = ((static_cast<size_t>(n_run) - 1) * eq_stride_elems + static_cast<size_t>(head.nof_re) * head.nof_layers) * 2 * sizeof(float);
    const size_t nv_bytes = ((static_cast<size_t>(n_run) - 1) * nv_stride_elems + static_cast<size_t>(head.nof_re) * head.nof_layers) * sizeof(float);

    std::vector<uint32_t> h_starts(n_run, 0);
    for (unsigned k = 0; k != n_run; ++k) {
      h_starts[k] = pending[first + k].h.offset;
    }
    wrapped_buffer b_starts{};
    if (n_run > 1) {
      b_starts.buffer = eq_bind_h_starts(h_starts.data(), n_run);
    }
    wrapped_buffer b_h = wrap_buffer(engine, h_run_binding.buffer, h_bytes);
    wrapped_buffer b_y = wrap_buffer(engine, y_alloc, y_bytes);
    wrapped_buffer b_s = wrap_buffer(engine, s_alloc, s_bytes);
    wrapped_buffer b_eq = wrap_buffer(engine, head.eq, eq_bytes);
    wrapped_buffer b_nv = wrap_buffer(engine, head.nv, nv_bytes);
    if ((b_h.buffer == nil) || (b_y.buffer == nil) || (b_s.buffer == nil) || (b_eq.buffer == nil) ||
        (b_nv.buffer == nil) || ((n_run > 1) && (b_starts.buffer == nil))) {
      engine->last_call_no_copy = false;
      compat::aligned_free(h_alloc);
      compat::aligned_free(y_alloc);
      compat::aligned_free(s_alloc);
      return nil;
    }
    engine->last_call_no_copy = true;

    // The received symbols: the gather dispatch reads them off the grid into the run's y region,
    // right before the equalization that consumes them. Both are dispatches of this same command
    // buffer and the gather has a pipeline of its own, so the burst's barrier between pipelines
    // orders the two - and the grid's producing FFT is ordered by the queue, since the demodulator
    // has already waited for every symbol of the slot before it reads the grid on the host for the
    // channel estimates.
    if (gather_run) {
      wrapped_buffer b_grid = wrap_buffer(engine, gather_plan->grid.base, grid_view_bytes(gather_plan->grid));
      if (b_grid.buffer == nil ||
          !eq_encode_gather_dispatch(enc,
                                     b_grid,
                                     b_y,
                                     *gather_plan,
                                     first_symbol,
                                     n_run,
                                     head.nof_ports,
                                     head.nof_re,
                                     eq_flush_state().hop_tables,
                                     eq_gather_taps(),
                                     eq_gather_entries())) {
        engine->last_call_no_copy = false;
        compat::aligned_free(h_alloc);
        compat::aligned_free(y_alloc);
        compat::aligned_free(s_alloc);
        return nil;
      }
    }

    // A single-symbol run keeps the per-symbol kernel: it is the path every caller already
    // validates, while the batched kernel is the one that has to prove itself with a group.
    id<MTLComputePipelineState> run_pipeline = (n_run > 1) ? eq_resources().pipeline_batch : eq_resources().pipeline;
    // The estimates are bound at the buffer base and p.h_offset carries the run's first estimate:
    // the table's starts are absolute within h_run_binding.buffer, and the kernel subtracts
    // p.h_offset from each. Binding h_run_binding.offset HERE as well applied that base twice, so
    // every run whose first estimate was not at 0 read its estimates from the wrong place.
    id<MTLBuffer> b_starts_run = (n_run > 1) ? b_starts.buffer : bind_one_h_start(head.h.offset);
    if (b_starts_run == nil) {
      return nil;
    }
    used_pipeline = eq_encode_batch_dispatch(enc,
                                             b_h,
                                             b_y,
                                             b_eq,
                                             b_nv,
                                             b_s,
                                             b_starts_run,
                                             params,
                                             strides,
                                             0u,
                                             head.nof_re,
                                             n_run);
    if (used_pipeline == nil) {
      return nil;
    }
    if (n_run > 1) {
      ++engine->batch_dispatches;
    }

    // The group staging stays alive until the command buffer that reads it completes: it is
    // recycled by the next flush, which the caller only reaches after waiting (see wait()).
    eq_flush_state().inflight.push_back(h_alloc);
    eq_flush_state().inflight.push_back(y_alloc);
    eq_flush_state().inflight.push_back(s_alloc);
    any_batch = true;
    first += n_run;
  }

  pending.clear();
  (void)any_batch;
  return used_pipeline;
}

} // namespace

bool equalizer_metal_engine::enqueue_burst(const ch_est_binding& h,
                                          bool                  h_on_device,
                                          const void*           y,
                                          const void*           sigma2,
                                          void*                 eq,
                                          void*                 nv,
                                          unsigned              nof_re,
                                          unsigned              nof_ports,
                                          unsigned              nof_layers,
                                          bool                  mmse,
                                          float                 noise_var,
                                          float                 tx_scaling,
                                          float                 h_scaling,
                                          const gather_binding& gather)
{
  eq_engine_impl* engine = static_cast<eq_engine_impl*>(impl);
  if (engine == nullptr) {
    return false;
  }
  // \name Two encodings of the same dispatch.
  ///
  /// The batched form (the default) accumulates the symbols of a group and lets the flush hook encode
  /// them as ONE dispatch per run when the burst changes stage (the demapping) or commits; the
  /// caller's inputs are read at that point, which is why they must stay alive until wait() - the
  /// contract the per-symbol path already has too.
  ///
  /// It reads each symbol's estimates at the start the ESTIMATOR published for it, not at a fixed
  /// stride: a symbol carrying DM-RS holds fewer data REs, so the starts step by 72, 108, 72, ...
  /// elements (see eq_batch_kernel_probe for the controlled comparison, and enqueue_burst_batch_at
  /// for the contract). Reading them at one stride is what made the first air legs fail - 95% of
  /// the PUSCH blocks came back in error and the handset never attached.
  ///
  /// Escape hatch: OCUDU_EQ_DEFER_ENCODE=0 restores the per-symbol encoding, which encodes each
  /// symbol where it is submitted. Both are bit-exact against each other: the recorded capture
  /// replays to the same 6380 LLR bytes either way, and the over-the-air link is healthy with the
  /// batched form (RRC setup completes, the session attaches, the ping goes through, and the PUSCH
  /// block error rate sits at the same level as the per-symbol chain).
  ///@{
  static const bool defer_encode = []() {
    const char* env = std::getenv("OCUDU_EQ_DEFER_ENCODE");
    return (env == nullptr) || (std::strtoul(env, nullptr, 10) != 0);
  }();
  if (defer_encode) {
    // A thread can accumulate for several engines over its lifetime (a worker creating one
    // demodulator after another). The burst only carries ONE pending hook, so work accumulated for
    // another engine must be handed over before this one installs its own - otherwise the new hook
    // would look for its own (empty) list and the earlier group would never reach the GPU.
    if ((metal::shared_burst::flush_hook_context() != nullptr) &&
        (metal::shared_burst::flush_hook_context() != engine)) {
      (void)metal::shared_burst::flush_pending();
    }
    eq_pending(engine).push_back({h,
                                  h_on_device,
                                  gather,
                                  y,
                                  sigma2,
                                  eq,
                                  nv,
                                  nof_re,
                                  nof_ports,
                                  nof_layers,
                                  mmse,
                                  noise_var,
                                  tx_scaling,
                                  h_scaling});
    metal::shared_burst::set_flush_hook(engine, &eq_flush_hook);
    return true;
  }
  {
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
  ///@}
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
  // Uniform layout: the per-symbol starts are h.offset plus a multiple of the stride.
  std::vector<unsigned> starts(nof_symbols + 1u, h.offset);
  for (unsigned k = 0; k != nof_symbols; ++k) {
    starts[k] = h.offset + k * h_symbol_stride;
  }
  // The estimates of the LAST symbol define how far the binding has to reach.
  const size_t h_bytes =
      ch_est_binding_bytes(h, nof_ports, nof_layers, nof_re) +
      static_cast<size_t>((nof_symbols > 1) ? (starts[nof_symbols - 1] - h.offset) : 0u) * sizeof(cbf16_t);
  ch_est_binding h_run = h;
  h_run.buffer         = h.buffer;
  return enqueue_burst_batch_at(h_run,
                                span<const unsigned>(starts.data(), nof_symbols),
                                y,
                                sigma2,
                                eq,
                                nv,
                                nof_re,
                                y_symbol_stride,
                                eq_symbol_stride,
                                nv_symbol_stride,
                                nof_ports,
                                nof_layers,
                                mmse,
                                noise_var,
                                tx_scaling,
                                h_scaling);
}

bool equalizer_metal_engine::enqueue_burst_batch_at(const ch_est_binding& h,
                                                    ocudu::span<const unsigned> h_starts,
                                                    const void*           y,
                                                    const void*           sigma2,
                                                    void*                 eq,
                                                    void*                 nv,
                                                    unsigned              nof_re,
                                                    unsigned              y_symbol_stride,
                                                    unsigned              eq_symbol_stride,
                                                    unsigned              nv_symbol_stride,
                                                    unsigned              nof_ports,
                                                    unsigned              nof_layers,
                                                    bool                  mmse,
                                                    float                 noise_var,
                                                    float                 tx_scaling,
                                                    float                 h_scaling,
                                                    const gather_binding& gather)
{
  const unsigned nof_symbols = static_cast<unsigned>(h_starts.size());
  if (nof_symbols == 0) {
    return false;
  }
  eq_engine_impl* engine = static_cast<eq_engine_impl*>(impl);
  if ((engine == nullptr) || (nof_symbols == 0)) {
    return false;
  }
  id<MTLComputeCommandEncoder> enc = metal::shared_burst::encoder(eq_resources().pipeline_batch);
  if (enc == nil) {
    return false;
  }
  // The bound estimate range reaches through the LAST symbol's own start: the starts are the
  // estimator's, so a stride cannot describe them.
  const size_t h_bytes = static_cast<size_t>(h_starts[nof_symbols - 1]) *
                             static_cast<size_t>(nof_ports) * nof_layers * sizeof(cbf16_t) +
                         static_cast<size_t>(nof_ports) * nof_layers * nof_re * sizeof(cbf16_t);
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
  eq_strides_t strides{nof_symbols, 0u, y_symbol_stride, eq_symbol_stride, nv_symbol_stride};
  [enc setBuffer:b_h.buffer offset:b_h.offset atIndex:0];
  [enc setBuffer:b_y.buffer offset:b_y.offset atIndex:1];
  [enc setBuffer:b_eq.buffer offset:b_eq.offset atIndex:2];
  [enc setBuffer:b_nv.buffer offset:b_nv.offset atIndex:3];
  [enc setBytes:&params length:sizeof(params) atIndex:4];
  [enc setBuffer:b_s.buffer offset:b_s.offset atIndex:5];
  id<MTLBuffer> b_starts = eq_bind_h_starts(h_starts.data(), nof_symbols);
  if (b_starts == nil) {
    engine->last_call_no_copy = false;
    return false;
  }
  [enc setBytes:&strides length:sizeof(strides) atIndex:6];
  [enc setBuffer:b_starts offset:0 atIndex:7];
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
  // The GPU-time probe must be armed before commit (Metal asserts otherwise).
  metal::shared_queue::arm_gpu_time(cmd_buf, metal::shared_queue::queue_kind::back_end);
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
  // The GPU-time probe must be armed before commit (Metal asserts otherwise).
  metal::shared_queue::arm_gpu_time(cmd_buf, metal::shared_queue::queue_kind::back_end);
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

equalizer_metal_engine::batch_diag equalizer_metal_engine::batch_diagnostics() const
{
  // One process-wide set of counters, like the [metal_stats] report: the flush hook is a free
  // function that a thread-local burst calls, so it has no engine instance to count on.
  batch_diag             d;
  const eq_batch_diag_t& c = eq_batch_diag();
  d.flushes      = c.flushes.load(std::memory_order_relaxed);
  d.symbols      = c.symbols.load(std::memory_order_relaxed);
  d.runs         = c.runs.load(std::memory_order_relaxed);
  d.batched_runs = c.batched_runs.load(std::memory_order_relaxed);
  d.max_run      = c.max_run.load(std::memory_order_relaxed);
  const char* brk = c.first_break.load(std::memory_order_relaxed);
  d.first_break  = (brk == nullptr) ? "" : brk;
  return d;
}

void equalizer_metal_engine::reset_batch_diagnostics()
{
  eq_batch_diag_t& c = eq_batch_diag();
  c.flushes.store(0, std::memory_order_relaxed);
  c.symbols.store(0, std::memory_order_relaxed);
  c.runs.store(0, std::memory_order_relaxed);
  c.batched_runs.store(0, std::memory_order_relaxed);
  c.max_run.store(0, std::memory_order_relaxed);
  c.first_break.store(nullptr, std::memory_order_relaxed);
}

double equalizer_metal_engine::last_gpu_wait_us() const
{
  const eq_engine_impl* engine = static_cast<const eq_engine_impl*>(impl);
  return engine != nullptr ? engine->last_gpu_us : 0.0;
}

} // namespace metal
} // namespace ocudu
