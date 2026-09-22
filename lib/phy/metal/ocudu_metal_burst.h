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

#include "ocudu_metal_lane_probe.h"

#include <cstddef>
#include <cstdint>
#include <functional>

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

  /// \brief What the command buffer this thread's burst commits CARRIES, for the lane probe's busy split.
  ///
  /// The split attributes a command buffer's whole GPU span to ONE stage (Metal gives no finer timestamps),
  /// so the label has to name what is actually in it. On the \c merged route that is the WHOLE hop - the
  /// front end's transforms, the estimator, the equalization and the demapping, adopted into one buffer -
  /// and calling it \c equalizer_demapper made `eq_demap` the sum of four stages, i.e. the very number a
  /// latency optimization would aim at (5.9.61).
  ///
  /// \note The default is \c equalizer_demapper, which is the truth on every other route (there the burst
  ///       really is the equalizer and the demapper). Whoever makes a buffer carry more says so here, at the
  ///       point where it does: the estimator, where its adopted buffer becomes the lane's burst.
  /// \note Per thread, like the burst itself, and reset by every commit.
  static void set_commit_label(gpu_lane_probe::stage which);

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
  ///
  /// \param[in] on_drop Runs if this deposit is DROPPED instead of claimed - replaced by a newer deposit for
  ///            the same address, or evicted over the bound. It exists for one job: a handed-over block that
  ///            nobody claims is never committed, so whatever the block was keeping alive for its dispatches
  ///            (the receiving chain's INPUT, see dft_metal_engine::retain_for_block()) has to be let go
  ///            here rather than at a completion that will never come. Called without the registry's lock
  ///            held, on the thread that dropped it.
  /// \param[in] generation Grid-production fence generation armed on \p cb (shared_queue::grid_ready_signal),
  ///            or 0 when the depositor armed none - a host reader then has nothing to wait for.
  static void deposit_released(const void*          grid_base,
                               uint64_t             slot,
                               id<MTLCommandBuffer> cb,
                               uint64_t             generation = 0,
                               std::function<void()> on_drop   = {});

  /// \brief Called to commit a block the registry has to drop - see deposit_released()'s note on late
  ///        commits. Installed by the engine that deposits (it knows what a commit owes: the GPU-time probe
  ///        and, for a front-end block, the chain it is published on).
  using drop_commit_fn = void (*)(void* command_buffer);
  static void set_drop_committer(drop_commit_fn fn);

  /// \brief Takes the command buffer deposited for \p grid_base, or nil when there is none.
  ///
  /// The caller becomes its submitter: it encodes its own stages into it (a second encoder - the deposit
  /// already carries the DFT's, ended) and commits it. No fences are encoded on it by this call: the
  /// buffer's FIRST dispatches are the ones that produced what the caller reads, which is the ordering.
  ///
  /// \note The deposit is NOT removed: it stays registered as CLAIMED until its command buffer completes, so
  ///       a HOST reader of that grid can still wait for its production (see ensure_grid_produced()). A
  ///       second take of the same address returns nil - the buffer belongs to one hop.
  static id<MTLCommandBuffer> take_released(const void* grid_base, uint64_t slot);

  /// \brief Makes sure the grid at \p grid_base has been - or will be - WRITTEN, for a host reader (D1-A).
  ///
  /// The consumers of the resource grid that read it on the HOST - the PUCCH is one, and it has no device
  /// view at all - cannot use a grid whose production the hand-over deferred to the lane's commit. This is
  /// where they wait, and it covers both shapes of a slot:
  ///
  ///  * a slot whose grid a hop CLAIMED: the lane commits it, and this waits for the grid-production fence
  ///    the release armed on that very buffer;
  ///  * a slot NOBODY claimed (a PUCCH-only slot, where no hop runs at all): nothing would ever commit it,
  ///    so this COMMITS it here - the fallback a hand-over owes whenever its consumer is not guaranteed.
  ///
  /// \note Call it from the consumer side and PROMPTLY: a deposit nobody claims is dropped when the address
  ///       comes back through the pool (a later slot's deposit supersedes it), and from then on the grid it
  ///       wrote cannot be produced at all. The counter that says it happened is `superseded`.
  /// \return True when the grid is ready (or nothing was pending); false when the wait timed out, which
  ///         means the reader must NOT trust the grid.
  static bool ensure_grid_produced(const void* grid_base, uint64_t slot);

  /// \brief The generation whose completion PRODUCES the grid at (\p grid_base, \p slot), for a consumer
  ///        that orders itself on the DEVICE instead of waiting on the host.
  ///
  /// Same record as ensure_grid_produced() and the same fallback - an unclaimed deposit is COMMITTED here,
  /// because a hand-over whose consumer is not a hop owes that - but what the caller does with the answer
  /// differs: it encodes the wait into its OWN command buffer (shared_queue::grid_ready_encode_wait) and
  /// returns. That is the whole point: the ordering is the same, and the CPU thread never blocks.
  ///
  /// Why it exists: a hop that MISSES the hand-over reads the grid from its own command buffer while the
  /// block is committed by someone else (another consumer's fallback, or the registry's sweep). Waiting for
  /// that on the HOST fixed the correctness and the median latency and then STALLED the lane - the waiter
  /// occupied a thread of the pool the committing stage runs on (measured: one 13-second incident, 355
  /// dropped uplink slots, design document 5.9.23).
  ///
  /// \return The generation to wait for, or 0 when there is nothing to wait for (no record for that
  ///         (storage, slot), or no fence armed on the block).
  static uint64_t grid_production_generation(const void* grid_base, uint64_t slot);

  /// \brief The grid production this thread's NEXT burst must wait for, before any of its dispatches.
  ///
  /// The burst creates its own command buffer (see burst_ensure_open()), so a caller that needs a
  /// command-buffer-level wait cannot encode it itself: it hands the generation over here, and the burst
  /// encodes it where it encodes its other command-buffer-level fences. Consumed once, when the buffer is
  /// created; a generation set while a burst is ALREADY open is counted by the caller as unencoded, because
  /// a wait encoded after dispatches cannot order them.
  ///
  /// \param[in] generation Value returned by grid_production_generation(); 0 clears it.
  static void set_grid_wait(uint64_t generation);

  /// Whether a grid wait is pending for this thread's next burst (diagnostics).
  static bool grid_wait_pending();

  /// \brief How many deposits the registry holds before it starts dropping them (see handed_counters).
  ///
  /// A record lives until its grid has been PRODUCED (so a late reader can be told "already written" rather
  /// than "unknown"), which is why this is a few slots of history rather than a handful of entries: at
  /// ~1000 slots/s, 256 covers a quarter of a second - far more than the one slot a consumer can be late by
  /// - and the bound still stops a hop that never runs from growing it.
  ///
  /// \note Exposed rather than kept file-local because a CRITERION has to be able to say what "the bound
  ///       fired" means without restating the number: the unit test asserts handed - evicted == this, and
  ///       evicted's own documentation refers to it. Measured on air the registry sits saturated here, so
  ///       that difference is exactly this value on every armed leg.
  ///
  /// \note \c OCUDU_D1_HANDED_BOUND overrides it, **for diagnosis only**, and exists for one arm: the
  ///       eviction branch below is reached on air on every armed leg, but the bound CANNOT be reached from
  ///       a unit test at its production value - each entry needs a real command buffer and Metal blocks at
  ///       about sixty uncommitted ones (measured), while the bound is 256. Lowering it is what makes that
  ///       branch, and the reverse arm that judges what the branch does, reachable offline. The production
  ///       default is unchanged.
  static size_t handed_capacity();

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
    /// Deposits the BOUND dropped, i.e. every one that arrived while max_handed were already outstanding.
    ///
    /// \note This is a TOTAL, and on its own it says nothing: measured on three armed legs the registry
    ///       sits permanently saturated at its own bound, so this equals `handed - max_handed` exactly
    ///       (30635/30379, 27221/26965, 28252/27996 with max_handed = 256) and counts approximately every
    ///       deposit. Judging a run by it - which an earlier revision of this comment invited, calling it
    ///       "the suspicious half" - makes every healthy leg look like a permanent backlog. The suspicious
    ///       half is `evicted_unproduced` below.
    uint64_t evicted = 0;
    /// \brief Entries evicted BEFORE anyone claimed or produced them.
    ///
    /// Was the suspicious half of `evicted`; since 5.9.62 it is **0 by construction**, because the removal
    /// loop only erases entries that have been PRODUCED - erasing an unproduced one is what could leave a
    /// reader with no record while the write was still in flight. Kept as a counter precisely because it now
    /// states an invariant: anything but 0 means that invariant broke.
    uint64_t evicted_unproduced = 0;
    /// \brief Times the registry was over its bound and had NOTHING safe to reclaim (5.9.62).
    ///
    /// The bound is soft now: while every outstanding entry is in flight there is nothing to erase without
    /// opening the hole above, so the loop stops and the completion handlers catch up. This is the pressure
    /// reading that replaces `evicted_unproduced` - a large value means consumers are far behind producers.
    uint64_t over_bound = 0;
    /// Records whose grid has NOT been produced yet (a block waiting for its consumer or its sweep). NOT
    /// "records held": the registry keeps a record after production so a late reader can be told so.
    size_t unproduced = 0;
    /// Deposits a host reader found still unclaimed and had to COMMIT itself (see ensure_grid_produced).
    /// Non-zero is normal in a run with PUCCH-only slots; a large number means the consumers are late and
    /// the deposits are being swept, not served.
    uint64_t fallback_commits = 0;
    /// Blocks COMMITTED LATE by the registry itself: deposits NOBODY claimed at all - either about to be
    /// dropped (the storage came back, or the bound was reached), or swept once the receiving chain had
    /// moved more than the sweep window past their slot. Each one is a grid that would otherwise never have
    /// been written AND a set of input references that would never have been released (5.9.17).
    uint64_t late_commits = 0;
    /// Reads that found NO record for their (storage, slot): either no hand-over is armed, or the record was
    /// produced and evicted long before. A reader that finds nothing cannot wait, so this counts the reads
    /// the key cannot protect.
    uint64_t grid_not_found = 0;
    /// Host waits that timed out: the grid the caller was about to read was NOT ready.
    uint64_t ready_timeouts = 0;
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
