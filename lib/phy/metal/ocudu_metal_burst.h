// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief One command buffer shared by the consecutive stages of a deferred burst.
///
/// The stages of a burst (equalization and then demapping) write and read the same memory through
/// different no-copy wrapped buffers. Metal's hazard tracking cannot relate two buffers that alias
/// the same memory, and the command buffers of one queue only start in submission order - they may
/// overlap - so a consumer must not rely on the queue to observe the producer's writes. Encoding
/// every dispatch of the burst into a single command buffer and inserting an explicit memory
/// barrier when the stage (the compute pipeline) changes orders the stages without an intermediate
/// CPU wait and without one command buffer commit per stage.
///
/// The state is thread local on purpose: the stages of one demodulation run on the same thread,
/// while several demodulations run at the same time on different threads and must not share a
/// command buffer.

#pragma once

#if !defined(__OBJC__)
#error "ocudu_metal_burst.h is only available to Objective-C++ translation units."
#endif

#import <Metal/Metal.h>

namespace ocudu {
namespace metal {

/// Shared command buffer of the stages of one deferred burst.
class shared_burst
{
public:
  /// Stage that appended a dispatch. The engines taking part in a burst also keep their own
  /// commit/wait counters, but those only account for their synchronous per-call path, so the burst
  /// probe attributes each dispatch to its stage to make the split visible.
  enum class stage { equalizer, demapper, channel_estimator, other };

  /// \brief Opens the burst if needed and returns its encoder, switching to \c pipeline.
  ///
  /// A memory barrier is inserted when the pipeline changes, i.e. between the stages of a burst, so
  /// the writes of the previous stage are visible to the dispatches of the next one.
  /// \return The encoder of the burst, or nil when the command buffer could not be created.
  static id<MTLComputeCommandEncoder> encoder(id<MTLComputePipelineState> pipeline);

  /// True when a burst is open (dispatches encoded, not committed yet).
  static bool open();

  /// \brief Ends the encoder and commits the burst without waiting.
  /// \return True when a burst was committed.
  static bool commit();

  /// \brief Waits for every command buffer committed by this thread's bursts.
  /// \return True when all of them completed successfully.
  static bool wait_committed();

  /// \brief Continues in a command buffer a stage already opened, instead of creating one (S13-P2/P3).
  ///
  /// The estimator's extraction opens the hop's command buffer and holds it for the rest of the hop
  /// (see mmse_engine's pilots_stage::hold_for_weights). With this call the stages that follow - the
  /// weights, then the equalizer and the demapper - go on encoding into THAT buffer, so the whole hop is
  /// ONE submission rather than one per stage group, and the lane's commit() covers all of them.
  ///
  /// Only the command buffer is taken over, and only when no burst is open yet:
  ///
  ///  * its encoder is opened by the first encoder() call, which is also where the stage barrier that
  ///    orders the adopter after the buffer's previous dispatches is inserted;
  ///  * the command-buffer-level FENCES that burst_ensure_open() would otherwise encode (the front-end
  ///    wait, and the back-end stage wait) are NOT encoded for an adopted buffer: it already carries the
  ///    fences of the stages inside it, and those stages are exactly what this burst would have waited
  ///    for.
  ///
  /// \param[in] cb The command buffer to continue in; it must not be committed yet.
  /// \return False when \p cb is nil or a burst is already open (the caller must then either encode into
  ///         that one or commit it first).
  static bool adopt(id<MTLCommandBuffer> cb);

  /// \brief Number of dispatches encoded in the open burst (diagnostics, and the mechanism check of the
  /// estimator's fused-lane unit test).
  ///
  /// Zero when no burst is open, and reset when the next one opens. It counts dispatches, not stages: the
  /// engine stages that call count_dispatch() once per stage are counted once, the equalizer and the
  /// demapper once per dispatch.
  static unsigned size();

  /// \brief Hands a command buffer over to the stage that will read the memory it wrote (D1 step 2).
  ///
  /// The DFT opens ONE command buffer per receiving slot, writes that slot's resource grid in it, and hands
  /// it over UNCOMMITTED (dft_metal_engine::release_block()). Its consumer - the channel estimator's
  /// extraction, the first back-end stage of the hop - runs on ANOTHER thread (the lower PHY's radio thread
  /// submits the transforms, the upper PHY's thread consumes the grid), so the two ends cannot meet in this
  /// thread's burst state. They meet here instead, keyed by the storage the buffer wrote: the resource
  /// grid's own base address.
  ///
  /// The key is what makes the pairing exact rather than lucky - a hop adopts the buffer that wrote the
  /// grid IT is about to read - and it is the same address on both sides by construction
  /// (resource_grid_writer::get_device_view() and resource_grid_reader::get_device_view() describe one
  /// storage).
  ///
  /// \note The registry is BOUNDED and keeps ONE deposit per address (a newer one replaces the older): a
  ///       deposit nobody takes is DROPPED, and that buffer is then never committed by anyone. It is
  ///       therefore armed only where a taker is guaranteed (see ofdm_demodulator_impl::finish_symbol():
  ///       the release needs the grid to be declared device-consumed) - a dropped deposit whose grid IS
  ///       read would be the P0 signature, not a slow hop. Both counts are reported (see handed_stats()).
  static void deposit_released(const void* grid_base, id<MTLCommandBuffer> cb);

  /// \brief Takes the command buffer deposited for \p grid_base, removing the deposit (nil when none).
  ///
  /// The caller becomes its submitter: it encodes its own stages into it (a second encoder - the deposit
  /// already carries the DFT's, ended) and commits it. No fences are encoded on it by this call: the
  /// buffer's FIRST dispatches are the ones that produced what the caller reads, which is the ordering.
  static id<MTLCommandBuffer> take_released(const void* grid_base);

  /// \brief What the registry has seen, for the diagnostics (see the [metal_stats] dft handover line).
  struct handed_counters {
    /// Deposits made.
    uint64_t handed = 0;
    /// Deposits claimed by the hop that reads that grid - the healthy path.
    uint64_t taken = 0;
    /// Deposits replaced because the SAME address was deposited again. The grid came back through the
    /// pool, which can only happen once its previous holder let it go - so the slot that deposited before
    /// had no consumer, and the grid nobody read is the reason. HARMLESS, and expected: it is how this
    /// counter says "that slot produced a block and no hop".
    uint64_t superseded = 0;
    /// Deposits dropped because more than max_handed were outstanding: a BACKLOG, i.e. consumers falling
    /// behind producers. This one is the suspicious half, and it is kept apart from `superseded` for
    /// exactly that reason - one number for both would make an expected outcome and a defect read alike.
    uint64_t evicted = 0;
    /// Deposits still unclaimed: the grid of a slot whose hop has not started (or never will).
    size_t outstanding = 0;
  };
  static handed_counters handed_stats();

  /// Accounts one dispatch appended to the burst (diagnostics).
  static void count_dispatch(stage which = stage::other);

  /// \brief Callback a stage registers to encode its deferred dispatches into the open burst.
  ///
  /// A stage that accumulates work (for example the equalizer, which can encode a whole group as
  /// one dispatch instead of one per symbol) must hand over the accumulated dispatches BEFORE the
  /// burst moves on: before the pipeline changes to the next stage (so the barrier that orders the
  /// stages still lands after them) and before the burst is committed. The hook returns the
  /// pipeline it encoded with, or nil when it encoded nothing, so the burst keeps its stage
  /// tracking correct.
  using flush_hook_t = id<MTLComputePipelineState> (*)(void* context, id<MTLComputeCommandEncoder> encoder);

  /// Registers the flush hook of the calling thread, together with its context. A hook registered
  /// by a different context while another one is pending is flushed first, so no accumulated work
  /// can be lost; pass a null hook to unregister.
  static void set_flush_hook(void* context, flush_hook_t hook);

  /// \brief Hand over the work a registered hook still has pending, keeping the burst's stage.
  ///
  /// A caller that is about to accumulate in a context DIFFERENT from the registered one must call
  /// this first: replacing (or bypassing) the hook of a context that still has work pending would
  /// drop that work silently - the next hook only ever encodes its own context. This is what makes
  /// a thread that runs several deferred engines one after another safe.
  /// \return The pipeline the pending hook encoded with, or nil when there was nothing to hand over.
  static id<MTLComputePipelineState> flush_pending();

  /// Context of the registered hook, or nullptr when none is registered (diagnostics).
  static void* flush_hook_context();
};

} // namespace metal
} // namespace ocudu
