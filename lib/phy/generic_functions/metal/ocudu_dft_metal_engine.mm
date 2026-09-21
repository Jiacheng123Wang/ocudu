// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu_dft_metal_engine.h"
#include "ocudu_metal_lane_probe.h"

#include "ocudu_metal_burst.h"
#include "ocudu_metal_queue.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/phy/phy_pipeline_contract.h"
#include "ocudu/phy/phy_pipeline_crossings.h"

#include "ocudu/support/executors/ul_pipeline_probe.h"
#include "ocudu/support/macos_compat.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
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
  /// Transforms encoded. Equal to commits while every transform gets its own command buffer; larger
  /// once a block of them shares one (see begin_block()), which is why the contract check counts
  /// transforms and not commits.
  std::atomic<uint64_t> transforms{0};
  std::atomic<uint64_t> waits{0};
  /// Peak PIPELINE DEPTH: how many of the batch slots held an un-waited transform at once. Tracked by
  /// dft_stats_note_depth() from the engine's own slot_pending[] flags - never from commits minus waits,
  /// which grows without bound now that the wait policy does not wait for every transform.
  std::atomic<uint64_t> in_flight_max{0};
  /// Transforms whose input came straight from the radio's int16 buffer instead of the engine's
  /// float2 ring (see grid_write::time_samples). Zero means every transform is staging its input on
  /// the host - either the caller never asks for it, or the engine refused the samples and the
  /// caller fell back, which it warns about once.
  std::atomic<uint64_t> radio_inputs{0};
  /// Wraps of this engine that could not be zero-copy and were staged instead (see wrap_buffer): the
  /// engine's own tables when they are not page aligned, or a caller's buffer the registry does not
  /// describe. Counted because a silent copy here is exactly how "the transform reads the radio
  /// buffer" stops being true without any counter saying so.
  std::atomic<uint64_t> wrap_copies{0};
  /// Blocks HANDED OVER instead of committed (see release_block()). Zero on every run that does not arm
  /// OCUDU_DFT_RELEASE_BLOCK, which is what makes "the factory path never takes this route" a counter and
  /// not a reading of the code.
  std::atomic<uint64_t> released{0};
  /// wait_slot() calls that named a slot whose transform went out with a released block. A correctly
  /// wired release run reads 0 here: the host wait it removes is the point of the change, so a non-zero
  /// value is a wiring defect (the wait was skipped, and the caller's data may not be there yet).
  std::atomic<uint64_t> released_waits{0};
};

static dft_stats_t& dft_stats()
{
  static dft_stats_t s;
  return s;
}

static void dft_stats_note_depth(uint64_t depth)
{
  dft_stats_t& s = dft_stats();
  uint64_t     prev = s.in_flight_max.load(std::memory_order_relaxed);
  while (depth > prev && !s.in_flight_max.compare_exchange_weak(prev, depth, std::memory_order_relaxed)) {
  }
}

static void dft_stats_commit(uint64_t nof_transforms)
{
  dft_stats_t& s = dft_stats();
  s.commits.fetch_add(1, std::memory_order_relaxed);
  s.transforms.fetch_add(nof_transforms, std::memory_order_relaxed);
}

static void dft_stats_wait()
{
  dft_stats_t& s = dft_stats();
  s.waits.fetch_add(1, std::memory_order_relaxed);
}

/// Counts one block handed over instead of committed (see release_block()).
static void dft_stats_release()
{
  dft_stats().released.fetch_add(1, std::memory_order_relaxed);
}

/// Counts one wait that named a slot whose transform went out with a released block: the wait was NOT
/// honoured, and the caller has to be told (see release_block()).
static void dft_stats_released_wait()
{
  dft_stats().released_waits.fetch_add(1, std::memory_order_relaxed);
}

/// Counts one transform whose input came straight from the radio's int16 buffer (the zero-copy
/// path). Wrapped like the commit/wait counters so the call site never names the struct: the
/// accessor only exists when the probe is compiled in.
static void dft_stats_radio_input()
{
  dft_stats().radio_inputs.fetch_add(1, std::memory_order_relaxed);
}

/// Counts one wrap that had to stage a copy (see dft_stats_t::wrap_copies).
static void dft_stats_wrap_copy()
{
  dft_stats().wrap_copies.fetch_add(1, std::memory_order_relaxed);
}

static void dft_stats_report()
{
  const dft_stats_t& s = dft_stats();
  std::fprintf(stderr,
               // slots_in_flight is the DEPTH OF THE PIPELINE (how many of the max_pipeline_depth slots
               // hold an un-waited transform), not commits minus waits: the wait policy stopped waiting for
               // every transform (see ofdm_demodulator_impl::finish_symbol()), so that difference grows
               // without bound and would read like a backlog that is not there.
               "[metal_stats] dft commits=%llu transforms=%llu waits=%llu slots_in_flight=%llu radio_inputs=%llu "
               "wrap_copies=%llu released=%llu released_waits=%llu\n",
               static_cast<unsigned long long>(s.commits.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.transforms.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.waits.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.in_flight_max.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.radio_inputs.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.wrap_copies.load(std::memory_order_relaxed)),
               // released / released_waits: the release path of D1 step 1. Both are 0 unless the run armed
               // OCUDU_DFT_RELEASE_BLOCK, and released_waits must stay 0 even then (see the struct).
               static_cast<unsigned long long>(s.released.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.released_waits.load(std::memory_order_relaxed)));

  // D1 step 2: the handover's own counters, printed by the ENGINE rather than by the registry's own
  // translation unit: the engine is in every build that can arm the release, so an armed leg always sees
  // the line - and a line with handed=0 is then a finding (the demodulator's guard refused, or no slot
  // reached its last symbol), not an absent instrument. A run that did not arm it and never handed
  // anything over stays silent.
  uint64_t handed = 0;
  uint64_t taken  = 0;
  uint64_t dropped = 0;
  metal::shared_burst::handed_stats(handed, taken, dropped);
  const char* armed = std::getenv("OCUDU_DFT_RELEASE_BLOCK");
  const bool  release_armed = (armed != nullptr) && (std::strtoul(armed, nullptr, 10) != 0);
  if (release_armed || (handed != 0) || (taken != 0) || (dropped != 0)) {
    std::fprintf(stderr,
                 "[metal_stats] dft handover handed=%llu taken=%llu dropped=%llu (armed=%d)\n",
                 static_cast<unsigned long long>(handed),
                 static_cast<unsigned long long>(taken),
                 static_cast<unsigned long long>(dropped),
                 static_cast<int>(release_armed));
  }
}
/// \brief Registers the transform input requirement: the transforms of this run read the radio's
/// int16 samples instead of a host-staged copy (S-7f-6f).
static void register_dft_contract_check()
{
  // Audited: takes its transform input from the radio's buffer by zero-copy mapping when it can,
  // stages a copy (counted below as a host -> device write) when it cannot, and never reads device
  // data back.
  phy_pipeline_crossings::declare_reporter("dft");

  register_phy_pipeline_check(
      {"dft radio inputs", []() -> std::optional<bool> {
         const dft_stats_t& s = dft_stats();
         // TRANSFORMS, not command buffers: a block of them shares one command buffer (begin_block()),
         // so counting commits here printed "240016 of 17145 transforms" on the first batched leg.
         const uint64_t transforms = s.transforms.load(std::memory_order_relaxed);
         const uint64_t radio      = s.radio_inputs.load(std::memory_order_relaxed);
         std::fprintf(stderr,
                      "%llu of %llu transforms read the radio buffer",
                      static_cast<unsigned long long>(radio),
                      static_cast<unsigned long long>(transforms));
         if ((transforms == 0) || !phy_pipeline_mode_registry::is_published() ||
             (phy_pipeline_mode_registry::get() == phy_pipeline_mode::cpu)) {
           // No Metal transform in this run, or a run that never claimed the offloaded pipeline (a
           // unit test or a tool exercises the engine directly): nothing to require of it.
           return std::nullopt;
         }
         // A handful of transforms of the same engine belong to other paths (the engine is shared);
         // none at all means the input is still being staged on the host for the whole run.
         return radio * 100 >= transforms * 99;
       }});
}

/// Registered once, on first use of the engine (see register_dft_contract_check()).
static const bool dft_contract_registered = []() {
  register_dft_contract_check();
  return true;
}();

#else  // OCUDU_METAL_STATS
static void dft_stats_note_depth(uint64_t /*depth*/) {}
static void dft_stats_commit(uint64_t /*nof_transforms*/ = 1) {}
static void dft_stats_wait() {}
static void dft_stats_wrap_copy() {}
static void dft_stats_radio_input() {}
static void dft_stats_release() {}
static void dft_stats_released_wait() {}
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
  id<MTLCommandBuffer> last_committed_cb = nil;
  /// Receiving slot the transforms being submitted belong to (see set_lane_slot()), and whether it was
  /// ever told: without a slot there is nothing to group the transforms by, and the probe is not fed
  /// (the offline tools submit transforms without a receiving slot at all).
  uint64_t lane_slot     = 0;
  bool     has_lane_slot = false;

  /// \name One command buffer for a block of transforms (see commit_open()).
  ///
  /// A caller whose samples arrive a BLOCK at a time - the receiving chain under the whole-slot policy
  /// gets a whole slot per receive call - hands the transforms of that block over here and they are
  /// encoded into ONE command buffer, committed when the block ends or when a wait forces it. What it
  /// saves is the per-command-buffer cost, measured at ~12.7us of GPU time whether the buffer carries
  /// one transform or fourteen (see the DFT unit test): the transform itself is under a microsecond.
  /// What it must never do is make a transform wait for samples that have not arrived: the caller opens
  /// and commits the block around the samples it already holds (see set_block_transforms()).
  ///@{
  id<MTLCommandBuffer>         open_cb  = nil;
  id<MTLComputeCommandEncoder> open_enc = nil;
  uint64_t                     open_transforms = 0;
  ///@}

  /// \name D1 step 1: the block handed over instead of committed (see release_block()).
  ///
  /// The released buffer is held STRONG until this engine releases the next block: the handle the caller
  /// gets is a +0 reference (the cast is a __bridge one), so without this the block would be deallocated
  /// between the release and the adoption that is supposed to take it over.
  ///@{
  id<MTLCommandBuffer> released_cb = nil;
  /// Slots whose transform went out with a released block. \c slot_pending stays SET (the host still must
  /// not read those outputs), which is what lets wait_slot() recognise the request and refuse it loudly
  /// rather than return as a satisfied wait.
  bool slot_released[16] = {};
  ///@}

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

/// \brief Whether this run asks for a block of transforms to share one command buffer.
///
/// DEFAULT ON since its leg confirmed it (48.191(g)): the front end's GPU time per slot went from 531.6
/// to 424.0us and the end-to-end [ul_pipeline] moved with it (-105us), with the contract, the red lines
/// and the back-end lane unchanged. OCUDU_DFT_OPEN_BLOCK=0 is the escape hatch.
static bool block_batching_requested()
{
  const char* env = std::getenv("OCUDU_DFT_OPEN_BLOCK");
  return (env == nullptr) || (std::strtoul(env, nullptr, 10) != 0);
}

/// \brief Whether a block is OPEN, i.e. the transforms being submitted belong to one command buffer.
static bool block_accumulating(const dft_engine_impl* e)
{
  return (e != nullptr) && (e->open_cb != nil);
}

/// \brief Records that \p cb carries the transform of \p slot (the per-slot ring bookkeeping).
///
/// One place because the release path adds a third piece of state to the pair: a slot whose transform went
/// out with a released block has to be recognised by wait_slot() (see release_block()), and a NEW
/// submission into that slot is precisely what ends that state - the slot no longer names the buffer the
/// engine handed over.
static void note_slot_submission(dft_engine_impl* e, unsigned slot, id<MTLCommandBuffer> cb)
{
  e->slot_cb[slot]       = cb;
  e->slot_pending[slot]  = true;
  e->slot_released[slot] = false;
}

/// \brief Whether this run asks the open block to be HANDED OVER instead of committed (D1 step 1).
///
/// DEFAULT OFF: \c OCUDU_DFT_RELEASE_BLOCK=1 is the only thing that arms it, so the factory chain behaves
/// exactly as before while nobody asks for the release (see release_block()). Read on every call rather
/// than cached, so the unit test can arm and disarm it around the arms it compares - which is also what
/// keeps the caller from having to know that the decision is taken at begin_block() time.
static bool block_release_requested()
{
  const char* env = std::getenv("OCUDU_DFT_RELEASE_BLOCK");
  return (env != nullptr) && (std::strtoul(env, nullptr, 10) != 0);
}

/// \brief Closes an open block's encoder without committing it, for the paths that drop the engine.
///
/// Releasing a command encoder without endEncoding ABORTS in the Metal validation layer, and that is not
/// hypothetical: the first on-air leg of the block batching crashed on ^C with "Command encoder released
/// without endEncoding", because stopping the stream leaves the block of the interrupted slot open. The
/// command buffer is dropped rather than committed - at this point nothing is going to read its grid -
/// which is the same choice shared_burst's thread state makes for an open burst.
static void discard_open_block(dft_engine_impl* e)
{
  if ((e != nullptr) && (e->open_enc != nil)) {
    [e->open_enc endEncoding];
  }
  if (e != nullptr) {
    e->open_enc         = nil;
    e->open_cb          = nil;
    e->open_transforms  = 0;
  }
}

/// \brief Encoder for the next dispatch: the open command buffer when one is accumulating, a fresh one otherwise.
/// \return False when no encoder could be created (the caller must not commit anything).
static bool encode_into(dft_engine_impl*                                 e,
                        id<MTLCommandBuffer> __strong*                   cb_out,
                        id<MTLComputeCommandEncoder> __strong*           enc_out)
{
  if (block_accumulating(e)) {
    *cb_out  = e->open_cb;
    *enc_out = e->open_enc;
    return true;
  }
  id<MTLCommandBuffer> cmd_buf = [dft_resources().queue commandBuffer];
  if (cmd_buf == nil) {
    return false;
  }
  id<MTLComputeCommandEncoder> enc = [cmd_buf computeCommandEncoder];
  if (enc == nil) {
    return false;
  }
  *cb_out  = cmd_buf;
  *enc_out = enc;
  return true;
}

/// \brief Closes and commits a command buffer of this engine, with everything a front-end commit owes.
///
/// One place on purpose: the GPU-time probe must be armed before the commit, the front-end fence signal
/// is a command-buffer level API that has to be encoded with the encoder already closed, and the commit
/// has to be published on the front-end chain so wait_all_committed() can drain it (a commit published
/// on the wrong chain would make the wait target another queue's command buffer).
static void commit_front_end(dft_engine_impl* e, id<MTLCommandBuffer> cb, uint64_t nof_transforms)
{
  metal::shared_queue::arm_gpu_time(cb, metal::shared_queue::queue_kind::front_end);
  metal::shared_queue::front_end_signal(cb);
  [cb commit];
  dft_stats_commit(nof_transforms);
  if (e->has_lane_slot) {
    metal::gpu_lane_probe::register_front_end_commit(cb, e->lane_slot);
  }
  metal::shared_queue::notify_commit(cb, metal::shared_queue::queue_kind::front_end);
  e->last_committed_cb = cb;
}


/// Reports a refused radio-input request ONCE and tells the caller to stage its own input.
///
/// Not an error: the caller (the OFDM demodulator) falls back to filling the engine's float2 ring,
/// which is what it did before the input could come from the radio buffer. It is worth one warning
/// because it means the zero-copy input is not happening for the whole run.
bool refuse_time_input(dft_engine_impl* engine, const dft_metal_engine::grid_write& write, const char* reason)
{
  static bool warned = false;
  if (!warned) {
    warned = true;
    ocudulog::fetch_basic_logger("PHY").warning(
        "Metal DFT: the transform input cannot be taken from the radio buffer ({}); the caller stages it on the host",
        reason);
  }
  (void)engine;
  (void)write;
  return false;
}

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
  // The mapping may never cover more than the allocation it starts in, and it must start on a page:
  // the length is rounded with the RUNTIME page size (4 KiB on Linux, 16 KiB on Apple Silicon - a
  // hard-coded 4096 both fails the alignment and overstates the block), and when the process-wide
  // registry knows which aligned_alloc block the pointer belongs to, the rounded length is clamped to
  // what is left of it. A pointer the registry does not describe keeps the historical behaviour (the
  // caller's length is taken at face value).
  void*  alloc_base = nullptr;
  size_t alloc_size = 0;
  bool   alloc_known = compat::describe_aligned_allocation(ptr, &alloc_base, &alloc_size);
  const size_t page = compat::page_size();
  size_t       aligned = 0;
  if (alloc_known) {
    const size_t offset = static_cast<size_t>(static_cast<const char*>(ptr) - static_cast<const char*>(alloc_base));
    const size_t usable = (alloc_size > offset) ? (alloc_size - offset) : 0;
    // The allocation is page rounded by construction, so the largest page multiple that fits in it is
    // its own remainder: round DOWN to it. Rounding UP past the allocation is the over-map the old code
    // did, and refusing instead is worse than that: a refusal falls back to a COPY, and this engine
    // hands out the grid - a buffer the GPU keeps writing to - so a cached copy is a grid the
    // demodulator reads but nobody ever writes (measured on air: garbage symbols, negative SINR, RLF).
    aligned = (usable / page) * page;
    if (aligned < length) {
      aligned = 0; // the request really does not fit in the allocation: stage a copy
    }
  } else {
    // A pointer the registry does not describe: the caller owns the contract (historical behaviour).
    aligned = ((length + page - 1) / page) * page;
  }
  id<MTLBuffer> buf = nil;
  if (aligned != 0) {
    buf = [dft_resources().device newBufferWithBytesNoCopy:(void*)ptr
                                                    length:aligned
                                                   options:MTLResourceStorageModeShared
                                               deallocator:nil];
  }
  size_t mapped = aligned;
  if (buf == nil) {
    dft_stats_wrap_copy();
    // The same event in the lane-wide counter. The local one stays because the "dft radio inputs"
    // check is stated in terms of it.
    phy_pipeline_crossings::count_host_write_site("dft: input copied to the device (wrap refused)", length);
    buf = [dft_resources().device newBufferWithBytes:ptr length:length options:MTLResourceStorageModeShared];
    // The COPY holds `length` bytes, not the page-rounded length: recording `aligned` here would let a
    // later, larger request (<= aligned) hit this cache entry and bind a buffer shorter than it reads -
    // the kernel would then read past the copy and produce garbage without anything failing.
    mapped = length;
  }
  engine->buffer_cache[ptr] = std::make_pair(buf, mapped);
  return buf;
}

/// \brief Maps the grid a transform writes: the process-wide cache on the release path, this engine's own
///        private one otherwise.
///
/// Why the release path may not use the private cache: two \c MTLBuffer objects over one address are
/// UNORDERED to Metal - a buffer-scope barrier does not relate them and neither does the encoder boundary
/// (measured 200/200: cases C..E and case G of wip/metal_alias_order.mm). The stages that adopt a released
/// block read the grid through \c shared_queue::wrap_no_copy(), which is keyed by address and hands out
/// the mapping that covers the request, so mapping the grid there too is what makes the adopter bind the
/// SAME object - and one object with an encoder boundary between the producer and the consumers IS ordered
/// (case F, 200/200). With the private cache the DFT would write an object nobody reads: the P0 signature,
/// silent wrong data.
///
/// \param[out] offset Byte offset of \p grid_base inside the returned mapping: the shared cache hands out
///             the mapping of the whole allocation, which may start below the grid.
/// \return The mapping, or nil when it could not be made - the release path REFUSES the dispatch then
///         rather than staging a copy, because a copied grid is a grid the GPU writes and the host reads
///         through a different memory (see wrap_buffer's note on the same trap).
static id<MTLBuffer> wrap_grid(dft_engine_impl* engine, const void* grid_base, size_t grid_bytes, size_t* offset)
{
  *offset = 0;
  if (!block_release_requested()) {
    return wrap_buffer(engine, grid_base, grid_bytes);
  }
  return metal::shared_queue::wrap_no_copy(metal::shared_queue::device(), grid_base, grid_bytes, offset);
}

} // namespace

dft_metal_engine::~dft_metal_engine()
{
  dft_engine_impl* engine = static_cast<dft_engine_impl*>(impl);
  if (engine != nullptr) {
    discard_open_block(engine);
    // A block that was handed over is NOT ours to close: the adopter commits it, and its dispatches are
    // already encoded (release_block() ended the encoder). Dropping the strong reference here is what the
    // engine owes - the ladder that adopted it holds it for as long as it needs it.
    engine->released_cb = nil;
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
  std::call_once(stats_atexit_flag, []() { std::atexit(dft_stats_report); });
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
        discard_open_block(engine);
        delete engine;
        impl = nullptr;
        return false;
      }
      // rief Which queue the front-end DFT commits on.
      ///
      /// DEFAULT: the front-end queue (shared_queue::queue()), which is what this engine has always used - the
      /// per-symbol producers have a queue of their own so that the DFT of a slot overlaps the back-end lane of
      /// the previous one.
      ///
      /// OCUDU_DFT_BACKEND_QUEUE=1 commits them on the BACK-END queue instead, which is the queue the receiving
      /// chain's late stages (the estimator and the lane burst) use. That is an EXPERIMENT, not a candidate: with
      /// both stages on one queue, submission order alone orders the DFT before the lane's burst, so the
      /// cross-queue fence (shared_queue::front_end_wait, encoded by the burst) stops being what provides the
      /// ordering - while still being encoded, so the two arms differ ONLY in whether the ordering crosses a
      /// queue. Its purpose is to price the fence, which is the precondition for design document 5.9's step 1:
      /// D1 wants the DFT on the lane's queue, and that change is only worth its cost if the cross-queue relation
      /// is what is expensive. If the two arms read the same, the fence is free and D1 becomes purely about the
      /// 1.83 CPU commits per hop it removes.
      auto dft_queue = []() {
        // D1 step 1: a block that may be RELEASED belongs to whoever commits it, and that is the lane, on
        // the back-end queue - a command buffer is bound to the queue that created it, so a block created
        // on the front-end queue could not be adopted into the lane's chain. Arming the release therefore
        // selects the queue as well; the two are one decision, not two knobs to keep in step.
        if (block_release_requested()) {
          std::fprintf(stderr,
                       "[dft_release] D1 step 1: the DFT's open block is handed over uncommitted "
                       "(OCUDU_DFT_RELEASE_BLOCK=1), so it is created on the BACK-END queue - the queue the "
                       "lane commits on\n");
          return metal::shared_queue::backend_queue();
        }
        const char* env = std::getenv("OCUDU_DFT_BACKEND_QUEUE");
        if ((env != nullptr) && (std::strtoul(env, nullptr, 10) != 0)) {
          std::fprintf(stderr,
                       "[dft_queue] EXPERIMENT: the front-end DFT commits on the BACK-END queue "
                       "(OCUDU_DFT_BACKEND_QUEUE=1)\n");
          return metal::shared_queue::backend_queue();
        }
        return metal::shared_queue::queue();
      };
      res.queue = dft_queue();

      NSString* lib_path = resolve_dft_metallib_path();
      if (lib_path == nil) {
        ocudulog::fetch_basic_logger("PHY").error(
            "Metal DFT: pre-compiled shader library 'ocudu_dft.metallib' not found (searched the configure-time "
            "path, next to the executable, and the working directory)");
        discard_open_block(engine);
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
        discard_open_block(engine);
        delete engine;
        impl = nullptr;
        return false;
      }
      id<MTLFunction> fn = [library newFunctionWithName:@"dft_dit"];
      if (fn == nil) {
        ocudulog::fetch_basic_logger("PHY").error("Metal DFT: kernel 'dft_dit' not found in the shader library");
        discard_open_block(engine);
        delete engine;
        impl = nullptr;
        return false;
      }
      res.pipeline = [res.device newComputePipelineStateWithFunction:fn error:&error];
      if (res.pipeline == nil) {
        ocudulog::fetch_basic_logger("PHY").error("Metal DFT: pipeline creation failed: {}",
                                                  error != nil ? error.localizedDescription.UTF8String : "nil error");
        discard_open_block(engine);
        delete engine;
        impl = nullptr;
        return false;
      }

      ocudulog::fetch_basic_logger("PHY").debug("Metal DFT: loaded pre-compiled shader library {}", lib_path.UTF8String);
    }
  }

  // Host-side twiddle table: N/2 entries of exp(-2*pi*i*k/N), page-aligned, zero-copy wrapped. The
  // alignment is the RUNTIME page size: a 4 KiB-aligned pointer is not page aligned where the page is
  // 16 KiB (Apple Silicon), and newBufferWithBytesNoCopy then refuses it - the table used to be copied
  // silently for that reason (the [metal_stats] dft wrap_copies counter now says so if it happens).
  const size_t page     = compat::page_size();
  const size_t tw_bytes = ((static_cast<size_t>(size / 2) * 2 * sizeof(float)) + page - 1) / page * page;
  void*        tw_mem   = nullptr;
  if (::posix_memalign(&tw_mem, page, tw_bytes) != 0 || tw_mem == nullptr) {
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
  const size_t perm_bytes_rounded = (static_cast<size_t>(perm_bytes) + page - 1) / page * page;
  void*        perm_mem   = nullptr;
  if (::posix_memalign(&perm_mem, page, perm_bytes_rounded) != 0 || perm_mem == nullptr) {
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
  engine->buf_perm = wrap_buffer(engine, perm_mem, perm_bytes_rounded);
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
      const size_t warmup_bytes = ((static_cast<size_t>(size) * 2 * sizeof(float)) + page - 1) / page * page;
      if (::posix_memalign(&engine->warmup_mem, page, warmup_bytes) == 0) {
        std::memset(engine->warmup_mem, 0, warmup_bytes);
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
    // While a block accumulates, the transform is in the OPEN buffer and nothing was committed yet.
    note_slot_submission(engine, slot, block_accumulating(engine) ? engine->open_cb : engine->last_committed_cb);
    // The pipeline depth the diagnostic reports: the slots that hold an un-waited transform. It is what
    // "in flight" means for this engine, and it stays bounded by max_batch_slots however few waits the
    // caller pays (see dft_stats_note_depth()).
    uint64_t depth = 0;
    for (unsigned i = 0; i != max_batch_slots; ++i) {
      depth += engine->slot_pending[i] ? 1u : 0u;
    }
    dft_stats_note_depth(depth);
  }
  return ok;
}

bool dft_metal_engine::begin_block()
{
  dft_engine_impl* engine = static_cast<dft_engine_impl*>(impl);
  if ((engine == nullptr) || !block_batching_requested()) {
    return false;
  }
  if (engine->open_cb != nil) {
    return true; // already open
  }
  // The block's command buffer is created when the caller says the block starts: from here until
  // commit_open() every transform is encoded into it.
  engine->open_cb = [dft_resources().queue commandBuffer];
  if (engine->open_cb == nil) {
    return false;
  }
  engine->open_enc = [engine->open_cb computeCommandEncoder];
  if (engine->open_enc == nil) {
    engine->open_cb = nil;
    return false;
  }
  engine->open_transforms = 0;
  return true;
}

bool dft_metal_engine::commit_open()
{
  dft_engine_impl* engine = static_cast<dft_engine_impl*>(impl);
  if ((engine == nullptr) || (engine->open_cb == nil) || (engine->open_enc == nil)) {
    return false;
  }
  id<MTLCommandBuffer>         cb  = engine->open_cb;
  id<MTLComputeCommandEncoder> enc = engine->open_enc;
  const uint64_t               nof = engine->open_transforms;
  engine->open_cb         = nil;
  engine->open_enc        = nil;
  engine->open_transforms = 0;
  [enc endEncoding];
  if (nof == 0) {
    // Nothing was encoded: the block produced no work, so there is nothing to commit. (The command
    // buffer is dropped; a transform that was refused by the caller never reached the engine.)
    return true;
  }
  commit_front_end(engine, cb, nof);
  return true;
}

bool dft_metal_engine::has_open() const
{
  const auto* engine = static_cast<const dft_engine_impl*>(impl);
  return (engine != nullptr) && (engine->open_cb != nil);
}

bool dft_metal_engine::block_release_enabled()
{
  return block_release_requested();
}

void* dft_metal_engine::release_block(const void* grid_base)
{
  dft_engine_impl* engine = static_cast<dft_engine_impl*>(impl);
  // DEFAULT OFF: without the knob this answers nullptr and NOTHING else happens - not even the encoder
  // is touched, so the factory path cannot be disturbed by the release path existing (D1 step 1's
  // criterion). The check comes first for exactly that reason.
  if ((engine == nullptr) || !block_release_requested() || (engine->open_cb == nil)) {
    return nullptr;
  }
  id<MTLCommandBuffer> cb = engine->open_cb;
  const uint64_t       nof = engine->open_transforms;
  // Close the encoder before handing over: the adopter opens its own (shared_burst::take_released() takes
  // the buffer only), and the boundary between the two encoders is what orders the adopter's first dispatch
  // after this block's dispatches for the SAME buffer object (see wrap_grid()).
  if (engine->open_enc != nil) {
    [engine->open_enc endEncoding];
  }
  engine->open_enc        = nil;
  engine->open_cb         = nil;
  engine->open_transforms = 0;
  if (nof == 0) {
    // Nothing was encoded: there is no work to hand over and the buffer is dropped, exactly as
    // commit_open() drops an empty block.
    return nullptr;
  }
  // The slots keep slot_pending set - the host still must not read their output - and are marked as
  // released, so a later wait_slot() is recognised and refused instead of quietly satisfied. The command
  // buffer itself is held STRONG: the handle below is a +0 reference (see the impl struct).
  for (unsigned i = 0; i != max_batch_slots; ++i) {
    if (engine->slot_cb[i] == cb) {
      engine->slot_released[i] = true;
    }
  }
  engine->released_cb = cb;
  dft_stats_release();
  // NOT commit_front_end(): no commit, no front-end fence signal, no front-end chain publication, no
  // dft commit counter. The caller submits this buffer, and everything a commit owes moves with it
  // (see the header).
  //
  // The deposit is what actually carries the buffer to its consumer, which runs on another thread and
  // looks it up by the grid it is about to read (shared_burst::deposit_released()).
  metal::shared_burst::deposit_released(grid_base, cb);
  return (__bridge void*) cb;
}

/// Reports a wait that cannot be honoured because the slot's transform was handed over. Once, loudly:
/// the caller that released the block promised the host would not read its output, so this is a wiring
/// defect and the data the caller is about to read may not be there yet.
static void report_released_wait(unsigned slot)
{
  dft_stats_released_wait();
  static bool reported = false;
  if (!reported) {
    reported = true;
    ocudulog::fetch_basic_logger("PHY").error(
        "Metal DFT: wait_slot({}) cannot be honoured - this slot's transform went out with a block that was "
        "handed over uncommitted (release_block()), so this engine no longer owns its submission. The caller "
        "that released the block must guarantee nothing on the host reads the slot's output before the "
        "adopter commits it; the wait is skipped, not satisfied",
        slot);
  }
}

bool dft_metal_engine::wait_slot(unsigned slot)
{
  dft_engine_impl* engine = static_cast<dft_engine_impl*>(impl);
  if ((engine == nullptr) || (slot >= max_batch_slots) || !engine->slot_pending[slot]) {
    return true;
  }
  if (engine->slot_released[slot]) {
    // Handed over (release_block()): there is no command buffer of this engine's to wait for - the
    // adopter commits it. Refuse instead of returning as a satisfied wait, which is what would let the
    // caller read a grid the GPU has not written yet without anything saying so.
    report_released_wait(slot);
    return false;
  }
  // The transform of this slot may still be sitting in the OPEN command buffer of its block: commit it
  // first, or the wait below would target a buffer that has not been committed at all.
  if (engine->open_cb != nil) {
    (void)commit_open();
  }
  id<MTLCommandBuffer> cmd_buf = engine->slot_cb[slot];
  engine->slot_pending[slot]   = false;
  engine->slot_cb[slot]        = nil;
  // Account for the slot wait so [metal_stats] reports the real in-flight depth (the ring keeps
  // up to `pipeline depth` transforms in flight instead of one).
  dft_stats_wait();
  // [ul_dft_wait]: the host time this wait costs. Records the WHOLE waitUntilCompleted, which is the instant the
  // synchronization actually occupies the caller - not the GPU span (last_gpu_us) of the buffer it waits for.
  const auto wait_begin = std::chrono::steady_clock::now();
  [cmd_buf waitUntilCompleted];
  ul_pipeline_probe::get().record_dft_wait(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - wait_begin).count());
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
  // \note Static by design (it drains the whole front-end chain, not one engine's work), so it cannot
  //       commit an open block itself: the caller does it first (see dft_processor_metal::wait()).
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
  // The grid goes through the cache its CONSUMERS use whenever the block may be handed over (see
  // wrap_grid): the offset is where the grid starts inside that mapping, and it travels to the kernel as
  // the buffer binding's offset - the kernel's own dst_offset stays relative to the grid.
  size_t        grid_off = 0;
  id<MTLBuffer> b_grid   = wrap_grid(engine, write.grid_base, write.grid_bytes, &grid_off);
  if (b_in == nil || b_out == nil || b_grid == nil) {
    return false;
  }

  // Input straight from the radio buffer (see grid_write::time_samples): the WHOLE allocation is
  // wrapped - it is the same pointer and the same length for every symbol of the slot, so the
  // mapping is created once - and the slice's offset travels to the kernel. A slice whose
  // allocation is unknown, or that does not fit in it, is refused: the caller stages its own input.
  struct {
    uint32_t is_ci16;
    uint32_t offset;
    float    gain;
    uint32_t pad;
  } input   = {0u, 0u, 1.0F, 0u};
  id<MTLBuffer> b_in16 = b_in; // a stand-in: the kernel only reads it when is_ci16 = 0
  if (write.time_samples != nullptr) {
    void*  alloc_base = nullptr;
    size_t alloc_size = 0;
    if (!compat::describe_aligned_allocation(write.time_samples, &alloc_base, &alloc_size)) {
      return refuse_time_input(engine, write, "the samples are not in a page-aligned allocation");
    }
    const size_t offset_bytes = static_cast<size_t>(static_cast<const char*>(write.time_samples) -
                                                    static_cast<const char*>(alloc_base));
    const size_t end_bytes =
        offset_bytes + std::max<size_t>(write.time_samples_bytes,
                                        (static_cast<size_t>(write.time_window_start) + engine->n) * 2 * sizeof(int16_t));
    if (end_bytes > alloc_size) {
      return refuse_time_input(engine, write, "the symbol does not fit in the allocation");
    }
    id<MTLBuffer> b = wrap_buffer(engine, alloc_base, alloc_size);
    if (b == nil) {
      return refuse_time_input(engine, write, "wrapping the radio buffer failed");
    }
    b_in16        = b;
    input.is_ci16 = 1u;
    dft_stats_radio_input();
    // The kernel reads from the ALLOCATION base it was handed, so the offset is the slice's own
    // offset plus the window start within it (the cyclic prefix the transform skips).
    input.offset = static_cast<uint32_t>(offset_bytes / (2 * sizeof(int16_t))) + write.time_window_start;
    input.gain   = write.time_gain;
  }

  id<MTLCommandBuffer>         cmd_buf = nil;
  id<MTLComputeCommandEncoder> enc     = nil;
  if (!encode_into(engine, &cmd_buf, &enc)) {
    return false;
  }
  [enc setComputePipelineState:dft_resources().pipeline];
  [enc setBuffer:b_in offset:0 atIndex:0];
  [enc setBuffer:b_out offset:0 atIndex:1];
  [enc setBuffer:engine->buf_tw offset:0 atIndex:2];
  [enc setBuffer:engine->buf_perm offset:0 atIndex:3];
  [enc setBytes:&engine->radix2 length:sizeof(uint32_t) atIndex:4];
  [enc setBytes:&engine->radix3 length:sizeof(uint32_t) atIndex:5];
  [enc setBytes:&engine->inverse length:sizeof(uint32_t) atIndex:6];
  // Element offset of the transform within the input it reads: the ring slot when the input is the
  // engine's float2 batch, and ZERO when it is the radio's buffer - that one holds this transform's
  // samples alone (the RX chain dispatches one transform per symbol), so the slot index does not
  // apply to it.
  const uint32_t base = (input.is_ci16 != 0u) ? 0u : (slot * engine->n);
  [enc setBytes:&base length:sizeof(uint32_t) atIndex:7];
  // The grid and its per-element table are only read when the write is active; Metal still requires every buffer the
  // kernel names to be bound, so the transform output and the twiddle table stand in when there is none.
  [enc setBuffer:b_grid offset:grid_off atIndex:8];
  [enc setBuffer:(engine->buf_window != nil ? engine->buf_window : engine->buf_tw) offset:0 atIndex:9];
  [enc setBuffer:b_in16 offset:0 atIndex:11];
  [enc setBytes:&input length:sizeof(input) atIndex:12];

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
  if (block_accumulating(engine)) {
    // The block's command buffer stays open: the transforms that arrive with it are encoded together and
    // the commit happens when the block ends (see commit_open()). The slot still records WHICH command
    // buffer carries its transform, so a wait for it commits the block first.
    ++engine->open_transforms;
    note_slot_submission(engine, slot, cmd_buf);
    return true;
  }
  [enc endEncoding];
  commit_front_end(engine, cmd_buf, 1);
  note_slot_submission(engine, slot, cmd_buf);
  return true;
}

void dft_metal_engine::set_lane_slot(uint64_t slot_index)
{
  dft_engine_impl* engine = static_cast<dft_engine_impl*>(impl);
  if (engine != nullptr) {
    engine->lane_slot     = slot_index;
    engine->has_lane_slot = true;
  }
}

uint64_t dft_metal_engine::fence_generation()
{
  return metal::shared_queue::front_end_generation();
}

uint64_t dft_metal_engine::fence_nof_signals()
{
  return metal::shared_queue::front_end_nof_signals();
}

uint64_t dft_metal_engine::fence_nof_waits()
{
  return metal::shared_queue::front_end_nof_waits();
}

uint64_t dft_metal_engine::fence_nof_skipped_waits()
{
  return metal::shared_queue::front_end_nof_skipped_waits();
}

bool dft_metal_engine::fence_selftest(bool& waited)
{
  waited = false;
  id<MTLCommandQueue> queue = metal::shared_queue::backend_queue();
  if (queue == nil) {
    return false;
  }
  id<MTLCommandBuffer> cb = [queue commandBuffer];
  if (cb == nil) {
    return false;
  }
  waited = metal::shared_queue::front_end_wait(cb);
  // A command buffer with only a wait would never run (Metal executes what its encoders encode), so the
  // test puts one real blit in it: the point is that a buffer carrying a fence wait still completes.
  id<MTLBuffer> src = [shared_queue::device() newBufferWithLength:64 options:MTLResourceStorageModeShared];
  id<MTLBuffer> dst = [shared_queue::device() newBufferWithLength:64 options:MTLResourceStorageModeShared];
  if ((src == nil) || (dst == nil)) {
    return false;
  }
  id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
  [blit copyFromBuffer:src sourceOffset:0 toBuffer:dst destinationOffset:0 size:64];
  [blit endEncoding];
  [cb commit];
  [cb waitUntilCompleted];
  return cb.status == MTLCommandBufferStatusCompleted;
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

  id<MTLCommandBuffer>         cmd_buf = nil;
  id<MTLComputeCommandEncoder> enc     = nil;
  if (!encode_into(engine, &cmd_buf, &enc)) {
    return false;
  }
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
  // The kernel names the radio-input arguments, so every dispatch has to bind them: the float2 input
  // stands in for the int16 one and the flag is off, which is exactly the pre-S-7f-6c behaviour.
  const struct {
    uint32_t is_ci16;
    uint32_t offset;
    float    gain;
    uint32_t pad;
  } input = {0u, 0u, 1.0F, 0u};
  [enc setBuffer:b_in offset:0 atIndex:11];
  [enc setBytes:&input length:sizeof(input) atIndex:12];
  [enc dispatchThreadgroups:MTLSizeMake(nof_transforms, 1, 1)
      threadsPerThreadgroup:MTLSizeMake(std::min(engine->n, 1024u), 1, 1)];
  if (block_accumulating(engine)) {
    // Part of a block: the encoder stays OPEN with the command buffer until the block ends (see
    // commit_open() - ending it here would close the buffer the next transform still has to encode
    // into). A caller that asked for accumulation must not ask for a completion wait on a single
    // transform either: there is nothing committed to wait for yet.
    ++engine->open_transforms;
    return true;
  }
  [enc endEncoding];
  commit_front_end(engine, cmd_buf, nof_transforms);
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
