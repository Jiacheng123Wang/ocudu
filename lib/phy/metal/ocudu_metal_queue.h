// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Process-wide Metal device and command queue shared by every PHY GPU engine.
///
/// \note Metal only serializes the *start* of the command buffers of one queue and is free to let
/// them overlap, and a buffer wrapped no-copy by one engine is a different resource than the same
/// memory wrapped by another one, so hazard tracking does not relate them either. A producer and a
/// consumer that share memory through the queue therefore have to synchronize explicitly: waiting
/// for the consumer's own command buffer does *not* guarantee that the producer's writes are
/// visible. Waiting for the newest command buffer of a burst does not even drain the older ones of
/// that same burst. The reliable options are a CPU-side wait (wait_all_committed() before touching
/// the memory, or the engine's own wait_committed(), which waits for every command buffer it has
/// committed) or a single command buffer with an explicit barrier between the encoders.
///
/// The intended usage for the CPU/GPU pipelining of the RX chain is:
///  - producer stage: submit work with the engine's non-waiting entry point (e.g.
///    dft_metal_engine::submit()) and keep the CPU busy with the next symbol,
///  - consumer stage: call wait_all_committed() before it reads the produced data from the CPU
///    (e.g. before staging the resource grid for the channel estimator), then dispatch normally.
///
/// \todo Move the remaining engines (channel estimator, equalizer, demapper, LDPC) onto this
///       queue when their stages are chained, and add the cross-stage command-buffer batching
///       planned in the pipeline decoupling work.

#pragma once

#if !defined(__OBJC__)
#error "ocudu_metal_queue.h is only available to Objective-C++ translation units."
#endif

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cstdint>

namespace ocudu {
namespace metal {

/// \brief Shared device and command queue, created on first use.
class shared_queue
{
public:
  /// Returns the process-wide Metal device, or nil when the platform has no Metal device.
  static id<MTLDevice> device();

  /// Returns the front-end command queue, used by the asynchronous producers of the RX chain (the
  /// per-symbol DFTs). nil when the device is unavailable or the queue could not be created.
  static id<MTLCommandQueue> queue();

  /// \brief Returns the back-end command queue, used by the late stages (channel estimator,
  /// equalizer, demapper, LDPC).
  ///
  /// The two queues keep the steadily fed front-end stream (a slot's worth of symbol DFTs submitted
  /// by the radio thread) from blocking the back-end stages: with one queue the back-end waits
  /// queue behind the in-flight FFTs and their measured latency exploded. Ordering across the two
  /// queues is provided by the explicit synchronization points instead of the queue itself - the
  /// RX chain drains and waits for a slot's DFTs before the grid is consumed, and every back-end
  /// stage waits for its own command buffer before its output is read on the CPU.
  static id<MTLCommandQueue> backend_queue();

  /// \brief Returns a no-copy buffer that wraps \p ptr for at least \p length bytes.
  ///
  /// The cache is process wide on purpose: Metal relates memory accesses (hazard tracking, memory
  /// barriers) through the *resource* they are bound to, so two engines that wrap the same memory
  /// separately would leave the producer/consumer dependency between their stages invisible to the
  /// driver. Handing out one buffer object per address makes the chained stages - for example the
  /// equalizer writing the symbols that the demapper reads - a tracked dependency again.
  ///
  /// \param[in] device Device that creates the buffer when the address is not cached yet.
  /// \param[in] ptr    Page-aligned base address.
  /// \param[in] length Number of bytes the caller needs (the mapping is page rounded).
  /// \param[out] offset When not null, receives the byte offset the requested range starts at inside
  ///             the returned buffer. Passing it also enables the CONTAINMENT lookup: a request that
  ///             falls inside a larger cached mapping returns that mapping with a non-zero offset
  ///             instead of a new object, so two stages that wrap different ranges of one allocation
  ///             (a group submit wrapping the whole group, a per-symbol stage wrapping a slice of it)
  ///             bind the SAME Metal buffer object - which is what relates their accesses. Two
  ///             objects over the same memory do not. Callers that do not ask for the offset keep
  ///             the exact-address behaviour.
  /// \return The buffer, or nil when the wrap failed and the caller must stage through a copy.
  static id<MTLBuffer> wrap_no_copy(id<MTLDevice> device, const void* ptr, size_t length, size_t* offset = nullptr);

  /// \brief Which of the two process-wide queues a commit belongs to.
  ///
  /// A wait can only speak about the command buffers of ONE queue: the two queues execute
  /// concurrently, so waiting for a command buffer of the other one says nothing about this one's
  /// work. The pending chain is therefore kept per queue, and a stage that publishes a commit must
  /// say which queue it committed on - a back-end commit published as a front-end one (or the other
  /// way round) would make the wait return before the work it was supposed to cover.
  enum class queue_kind {
    /// The front-end queue (see queue()): the per-symbol producers.
    front_end,
    /// The back-end queue (see backend_queue()): the late stages of the RX chain.
    back_end
  };

  /// \brief Registers a command buffer committed through one of the shared queues.
  ///
  /// Called by the engines right after commit() so that wait_all_committed() knows what to wait
  /// for and the [metal_stats] probe can account for the shared dispatches.
  static void notify_commit(id<MTLCommandBuffer> command_buffer, queue_kind kind);

  /// \brief Arms the GPU-time probe on a command buffer, right BEFORE it is committed.
  ///
  /// Metal requires a completed handler to be installed before commit() (installing it afterwards is an
  /// assertion failure), so the engines call this immediately before committing and notify_commit()
  /// after. Statistics builds only: without the probe this is a no-op and the submit path pays nothing.
  static void arm_gpu_time(id<MTLCommandBuffer> command_buffer, queue_kind kind);

  /// \brief Reports a wrap request whose slice offset does not satisfy the alignment its binding needs.
  ///
  /// The wrapped slice travels to the kernel as an offset into the mapped allocation, and a Metal
  /// binding requires that offset to be a multiple of the argument's element size (a `device const
  /// float2*` wants 8 bytes, a `float` 4, a `char` 1). Page alignment of the *base* is guaranteed by
  /// the allocation (see baseband_gateway_buffer_dynamic_aligned), the element alignment of a *slice*
  /// is not: an engine that cannot satisfy it must stage a copy instead of binding, and say so here -
  /// this counter is part of the "zero-copy wraps" contract check.
  static void notify_wrap_misaligned();

  // NOTE (5.9.65, user ruling A): the FRONT-END fence lived here. It related the front-end queue's DFTs to
  // the back-end readers of the grid, as step 2a of the design document's 48.189(c) - its whole purpose was
  // to make the host wait in ofdm_symbol_demodulator_impl::finish_symbol() removable (step 2b).
  //
  // It was retired because the hand-over became the DEFAULT: with the block handed over, the DFT's
  // transforms ride the LANE's command buffer, so (a) the front-end queue has no commit of that slot to
  // relate anything to, and (b) the ordering a grid consumer needs is carried by the GRID generation
  // (grid_ready_signal/grid_ready_wait), which is a different mechanism with its own event. Enabling the
  // fence and shipping the hand-over were therefore mutually exclusive routes, and this one was chosen.
  //
  // It was always opt-in and every air leg ever run reports signals=0 waits=0 generation=0, so nothing
  // ever depended on it: the unarmed path is ordered by the host wait that step 2b would have removed, and
  // the armed path by the grid generation. The back-end STAGE fence below is unrelated and stays.

  /// \brief Back-end stage fence (S-7g-19, Step 1'): relates the channel estimator's own command buffer
  /// to the lane burst that reads what it wrote.
  ///
  /// The estimator commits its weights into a command buffer of its own as soon as they are encoded, so
  /// that its GPU work overlaps the host encoding the equalization and the demapping of the same group -
  /// the overlap the shared-burst route gave up (measured on air as +125us of [ul_equalization_demod]).
  /// Its consumer, the lane burst, is a SECOND command buffer on the SAME queue, and one queue only
  /// orders the STARTS of its command buffers: nothing guarantees that the equalizer's dispatches, which
  /// read the weights and the noise variance the estimator wrote, run after the estimator's. This event
  /// is that guarantee, and it is the EXACT dependency: the lane burst waits for the estimator command
  /// buffer it reads, not for "the newest work on the back end" (the mistake the front-end fence made,
  /// see 48.189(f)).
  ///
  /// Same generation discipline as the front-end fence above: the estimator takes the generation and
  /// encodes its signal immediately before committing, so a signalled generation always has a command
  /// buffer on its way; the lane burst waits for the newest generation that existed when it was created,
  /// so it can never wait for a signal nobody will send. A burst created with no estimator commit at all
  /// (the unit tests, the replay tool, or a route whose estimator runs synchronously) encodes no wait.
  ///
  /// \note Only the route that commits early signals (see mmse_engine::set_lane_order()): a route that
  /// waits for its own command buffer has already ordered itself, and a wait for a stale generation is
  /// a no-op rather than an over-wait.
  static uint64_t backend_stage_signal(id<MTLCommandBuffer> command_buffer);

  /// \brief Encodes a wait for the newest COMMITTED estimator generation on \p command_buffer.
  ///
  /// Must be called while no encoder of that command buffer is open (it is a command-buffer level API),
  /// which is why the lane burst does it right after creating its command buffer.
  /// \return True when a wait was encoded; false when nothing was signalled yet.
  static bool backend_stage_wait(id<MTLCommandBuffer> command_buffer);

  /// The newest estimator generation whose signal has been encoded (0 before the first one).
  static uint64_t backend_stage_generation();

  /// \name Q9-C: WHICH generation a stage-fence wait named (dev doc 6.14).
  ///
  /// The measurement that exposed the second stall: the lane burst used to wait for the NEWEST generation
  /// that existed when it was created, and with two lane threads the newest can belong to the OTHER lane's
  /// estimator - whose signal is encoded in a command buffer that may reach the same (serial) back-end queue
  /// AFTER the burst that is waiting for it. A wait for a value whose signaller is behind it in its own queue
  /// cannot be satisfied until that signaller runs, and the signaller cannot run until the waiter does: on
  /// leg `p09-conc2` the lane probe measured the resulting stall as `commit->start = 5.0028 s` with
  /// `start->end = 1.24 ms` - the queue, not the device - on the merged-hop command buffers of three slots,
  /// all released within 400 us of each other, while the receive pool stayed empty and the radio parked.
  ///
  /// `own` is the count of waits that named the generation the caller's OWN estimator handed out for this
  /// hop (which by construction was committed before the burst), `newest` the ones that had to fall back to
  /// the global newest (a hop with no estimator signal of its own on this thread - the synchronous routes),
  /// and `cross_lane` how often the newest differed from the caller's own at that moment, i.e. how often the
  /// OLD rule would have waited for a foreign generation. `cross_lane` is an UPPER BOUND on the hazard, not
  /// a count of deadlocks: a foreign generation that is already committed is ahead in the queue and harmless.
  ///@{
  static void     note_stage_fence_wait(bool own_generation, bool crossed);
  static uint64_t nof_stage_fence_own_waits();
  static uint64_t nof_stage_fence_newest_waits();
  static uint64_t nof_stage_fence_cross_lane();
  ///@}

  /// \brief Encodes a wait for ONE named estimator generation (not the newest).
  ///
  /// Needed when a stage is ordered against SEVERAL submissions that were committed one after another:
  /// waiting for the newest would only cover the last of them, because two command buffers of one queue
  /// have their STARTS alone ordered (see backend_stage_wait). Same rule as that one: no encoder may be
  /// open when this is called, and the caller must have armed the signal already, or the wait hangs.
  /// \return True when a wait was encoded; false when the event does not exist or the value is 0.
  static bool backend_stage_wait_generation(id<MTLCommandBuffer> command_buffer, uint64_t generation);

  /// Diagnostics: signals encoded, waits encoded, and waits skipped because nothing was signalled.
  static uint64_t backend_stage_nof_signals();
  static uint64_t backend_stage_nof_waits();
  static uint64_t backend_stage_nof_skipped_waits();

  /// \brief Waits for every command buffer committed through \p kind's queue so far.
  ///
  /// \note Command buffers of one queue complete in submission order, so waiting for the most
  /// recently committed one drains the whole chain of that queue. Returns false when a command
  /// buffer failed, and true when there is nothing to wait for.
  static bool wait_all_committed(queue_kind kind);

  /// \brief Grid-production fence: tells a HOST reader of the resource grid when that grid has been written.
  ///
  /// With the block hand-over (D1) the resource grid is produced at the LANE's commit instead of at the end
  /// of the receiving slot, so a consumer that reads the grid on the HOST - the PUCCH is one, with no device
  /// view at all - would read memory nobody has written yet (measured: `metric=nan sinr=-inf` on every PUCCH
  /// report, design document 5.9.12). This is the wait those consumers owe: the hand-over's owner arms the
  /// signal on the command buffer that will carry the grid, and the consumer waits for the generation.
  ///
  /// Why an event and not the command buffer: at the moment a consumer asks, the buffer may still be open in
  /// the lane (encoded into, not committed), so `waitUntilCompleted` would be invalid. A generation wait
  /// works whenever the commit happens, and it is the same discipline as the front-end fence above.
  ///
  /// \param[in] command_buffer The buffer that will produce the grid; must not be committed yet.
  /// \return The generation to wait for, or 0 when the fence is unavailable (no device).
  static uint64_t grid_ready_signal(id<MTLCommandBuffer> command_buffer);

  /// \brief Waits (on the HOST, with a bound) for \p generation of the grid-production fence.
  ///
  /// \param[in] generation Value returned by grid_ready_signal(); 0 waits for nothing.
  /// \param[in] timeout_ms Upper bound, so a generation nobody signals cannot hang the caller for good.
  /// \return True when the generation was reached.
  static bool grid_ready_wait(uint64_t generation, uint32_t timeout_ms);

  /// \brief Encodes a wait on \p generation of the grid-production fence into \p command_buffer.
  ///
  /// The DEVICE-side form of grid_ready_wait(), for a consumer whose read of the grid is itself a DISPATCH:
  /// the dispatches encoded after it do not start until the block that produces that grid has completed, and
  /// the calling thread returns immediately. It exists because waiting on the host for that production is a
  /// CPU participation point that also BLOCKS a lane thread while the commit it waits for may need another
  /// lane stage: measured on air as one 13-second stall with 355 dropped uplink slots (design document
  /// 5.9.23). Nothing else about the ordering changes - the command buffer is submitted as before, so the
  /// grid is still produced before it is read.
  ///
  /// \param[in] command_buffer Buffer to encode into; must NOT have an encoder open (command-buffer level).
  /// \param[in] generation Value returned by grid_ready_signal(); 0 encodes nothing.
  /// \return True when the wait was encoded (false: nothing to wait for, or no fence in this process - a
  ///         generation nobody will signal must never be waited for, or the command buffer would hang).
  static bool grid_ready_encode_wait(id<MTLCommandBuffer> command_buffer, uint64_t generation);

  /// Number of command buffers committed through the shared queue (diagnostics).
  static uint64_t nof_commits();

  /// Number of command buffers still not waited for (diagnostics).
  static uint64_t nof_pending();
};

} // namespace metal
} // namespace ocudu
