// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief C++ front end of the Metal DFT (FFT) engine (Objective-C++ implementation in
/// ocudu_dft_metal_engine.mm): loads the precompiled ocudu_dft.metallib and runs the
/// iterative mixed-radix (2^k * 3^m) DIT kernel. One (size, direction) per engine
/// instance; the device, command queue and pipeline state are shared process-wide
/// across all instances.
///
/// The engine follows the same conventions as the LDPC/MMSE Metal engines: zero-copy
/// wraps of the host buffers (page-aligned, cached by pointer with a length check),
/// one command buffer per transform with a synchronous wait (the chainable encode-only
/// variant arrives with the single-command-buffer pipeline work), a construction-time
/// warm-up dispatch, and the process-wide commit/wait stats probe.

#pragma once

#include <cstddef>
#include <cstdint>

namespace ocudu {
namespace metal {

class dft_metal_engine
{
public:
  dft_metal_engine()  = default;
  ~dft_metal_engine();

  dft_metal_engine(const dft_metal_engine&)            = delete;
  dft_metal_engine& operator=(const dft_metal_engine&) = delete;

  /// Maximum transform size supported by the kernel (the threadgroup memory budget).
  static constexpr unsigned max_size = 4096;

  /// \brief One-shot setup: shared device/queue/pipeline, host-side twiddle table, the
  /// mixed-radix digit-reversal permutation table and the warm-up dispatch.
  /// \param[in] size    Transform size (2^k * 3^m, 2..max_size).
  /// \param[in] inverse True for the inverse transform (conjugated twiddles, unnormalized).
  /// \return True on success.
  bool init(unsigned size, bool inverse);

  /// \brief Synchronous transform of one buffer pair.
  /// \param[in]  in  Input, \c size complex floats, page-aligned, engine lifetime.
  /// \param[out] out Output, \c size complex floats, page-aligned, engine lifetime.
  /// \return True on success.
  /// \brief Executes \c nof_transforms independent transforms over the contiguous input buffer.
  /// \param[in]  in             Input samples, nof_transforms * size complex samples.
  /// \param[out] out            Output samples, same layout.
  /// \param[in]  nof_transforms Number of transforms (>= 1). Each transform is dispatched as its
  ///                            own threadgroup, so concurrent transforms use different GPU cores.
  /// \return True on success.
  bool run(const void* in, void* out, unsigned nof_transforms);

  /// \brief Commits \c nof_transforms transforms without waiting for completion.
  ///
  /// The outputs are only valid once the shared queue is synchronized: either by a later engine
  /// dispatching on the same queue (command buffers of one queue run in submission order) or by an
  /// explicit metal::shared_queue::wait_all_committed(queue_kind::front_end) before the data is read
  /// on the CPU (it drains this queue only - see shared_queue::queue_kind).
  /// This is the entry point used by the CPU/GPU pipelined RX chain: the per-symbol DFTs are
  /// submitted without stalling the CPU, and the consumer stage synchronizes once.
  /// \return True when the dispatch was encoded and committed.
  bool submit(const void* in, void* out, unsigned nof_transforms);

  /// \brief One command buffer for the transforms of one block of samples (see commit_open()).
  ///
  /// A caller that holds a whole block of samples - the receiving chain under the default whole-slot
  /// receive policy gets a whole slot per call - can hand its transforms over as a block instead of one
  /// command buffer each: begin_block() opens the accumulation, every transform submitted while it is
  /// open is encoded into one command buffer, and commit_open() closes and commits it. What that saves
  /// is the per-command-buffer cost on the GPU timeline, which the DFT unit test measures at ~12.7us
  /// whether the buffer carries one transform or fourteen (the transform itself is under a microsecond).
  ///
  /// \note The rule the caller must keep: **only samples that have already arrived may be batched**. The
  ///       per-symbol submission exists so that a symbol is transformed as soon as its samples are there
  ///       (see lower_phy_baseband_processor::ul_process); opening a block across samples still being
  ///       waited for would trade a command buffer for a stall. Both calls are no-ops - begin_block()
  ///       answers false - when the caller asks for the historical per-transform command buffers with
  ///       OCUDU_DFT_OPEN_BLOCK=0 (the accumulation is the default, see 48.191(g)).
  /// \return begin_block(): whether the accumulation is open. commit_open(): whether a buffer was committed.
  ///@{
  bool begin_block();
  bool commit_open();
  ///@}

  /// Whether a block of transforms is currently being accumulated.
  bool has_open() const;

  /// \brief A piece of host state that has to outlive the dispatch that READS it (D1 step 3, 5.9.7).
  ///
  /// The receiving chain's transform input is the RADIO's zero-copy buffer, and what tells the radio that a
  /// symbol's samples may be recycled is `finish_symbol()` returning: puxch_processor_impl::
  /// finish_oldest_symbol() retires the receive-buffer handle right after it. That is true today only
  /// because the host waits for this engine there. A block that is HANDED OVER executes later - at the
  /// lane's commit - so without a token the transforms would read samples the radio has already
  /// overwritten. Measured on air: `crc=KO 942 / OK 46` at `sinr=37.6 dB`, i.e. silent wrong data
  /// (design document 5.9.7).
  ///
  /// `release(context)` is called **exactly once**, either when the block's command buffer completes on the
  /// device or when the block is definitively dropped (it was never claimed, so it will never run). It runs
  /// on a **Metal completion thread**: the caller's release must be thread-safe, or must only hand the work
  /// back to the thread that owns the state.
  struct keep_alive {
    void (*release)(void* context) = nullptr;
    void* context                  = nullptr;
  };

  /// \brief Holds \p token until the block that is open NOW is done with it.
  ///
  /// The receiving chain attaches one per piece of input the open block reads - the handle of the samples
  /// the symbol it is submitting was demodulated from. A token attached to a block that is then COMMITTED
  /// (rather than handed over) is released on that commit's completion, so both paths hold the input for
  /// exactly as long as the dispatches that read it.
  ///
  /// \note Nothing is retained when no block is open: the caller keeps ownership, which is what leaves the
  ///       factory path (no block batching) exactly as it was.
  /// \note WHEN it is released is P2-E's question: by default at the command buffer's completion, and with
  ///       early_token_release_enabled() at the last dispatch that reads it - which on the fused lane is the
  ///       difference between holding the input for the front end's own work and holding it for the whole hop.
  /// \return True when the token is now this engine's to release.
  bool retain_for_block(const keep_alive& token);

  /// \brief Whether this run releases the block's tokens at the block's LAST INPUT-READING DISPATCH instead of
  /// at its command buffer's completion (P2-E): \c OCUDU_DFT_RELEASE_TOKENS_EARLY, **default OFF**.
  ///
  /// The tokens exist to keep the transform's input alive, and only the FRONT END reads it: the estimator and
  /// everything after it read the resource grid the front end wrote. The completion they are armed on, though,
  /// is the COMMAND BUFFER's - and after D1 that buffer is the WHOLE HOP, so the input is held for the whole
  /// hop's span (design document 5.9.129 (2)): on n1, ~5.25 ms instead of the front end's own few tens of
  /// microseconds. The receiving chain pays for that hold in the radio's receive pool, which is the
  /// backpressure its receive loop blocks on (`[ul_rx_pool] held_max/starved_events`).
  ///
  /// With the switch on, the block signals a shared event right after its encoder is closed - i.e. after every
  /// dispatch that reads the input, and before the adopter's first one - and the tokens are released by that
  /// event. The completion handler STAYS as the fallback, so a block whose signal never arrives (a failed
  /// buffer, a handover nobody commits) cannot leak its input.
  ///
  /// \warning MEASURED INACTIVE ON macOS 26.6.2 / Apple Silicon (2026-09-25). That platform publishes a
  ///          `MTLSharedEvent` signal encoded after an encoder has been created only when the COMMAND BUFFER
  ///          completes - measured three ways (a host poll of `signaledValue` while the buffer ran, the
  ///          delivery time of the listener's block, and a second command buffer waiting on the event), with
  ///          the control that a signal encoded BEFORE the first encoder is published immediately (~2 ms into
  ///          an 81 ms buffer). So on this machine the switch encodes the signal and the release still happens
  ///          at the completion: `token_release_stats()` / the `[metal_stats] dft handover ... tokens_early=`
  ///          line reports `signals>0, by_event=0, by_complete=N`, and the input keeps being held for the
  ///          whole hop. The mechanism is kept because it is what the documented semantics promise and it is
  ///          one environment variable away on a platform that honours them - but DO NOT read a leg's pool
  ///          numbers as "the hold does not matter" unless `by_event > 0` says the release actually moved.
  static bool early_token_release_enabled();

  /// \brief How many early token-release signals were encoded, and which end released the tokens (P2-E).
  ///
  /// Per SET of tokens (one per block that carried input), not per token: the question these answer is which
  /// of the two paths won the race for a block, and every token of a block travels together.
  struct token_release_stats_t {
    /// Early signals encoded into a block (0 on a run with the switch off).
    uint64_t early_signals = 0;
    /// Blocks whose tokens were released by the front end's event - the mechanism working.
    uint64_t by_event = 0;
    /// Blocks whose tokens were released by a command buffer's completion (or a drop) instead.
    uint64_t by_complete = 0;
  };
  static token_release_stats_t token_release_stats();

  /// \brief Q9-F4 (dev doc 6.30): how many front-end dispatches carried MORE THAN ONE transform, and how
  /// many transforms they carried in total.
  ///
  /// The batched front end (OCUDU_DFT_BATCH_SYMBOLS=N, **DEFAULT AUTO = one slot's own symbol count**) DEFERS the transforms of an open
  /// block and encodes them as one dispatch of N threadgroups: offline the same 14 n=768 transforms cost
  /// 171us of device window as 14 single-threadgroup dispatches and 13.75us as one, because a
  /// single-threadgroup dispatch is latency-bound and does not overlap its neighbours.
  ///
  /// `cap` is the EFFECTIVE number of transforms one dispatch may carry in this process (1 = no batching),
  /// `told_symbols` what the receiving chain said one slot carries (0 = never told), and `override_value` the
  /// knob's own value (0 = AUTO, 1 = the per-symbol CONTROL arm, N >= 2 = an explicit cap). A reading of
  /// `cap > 1` next to `dispatches == 0` says the deferral never happened - a finding rather than a silent
  /// no-op. Read by the self-test (arm 17) and printed on the `[metal_stats] dft` line.
  struct batch_stats_t {
    uint64_t dispatches    = 0;
    uint64_t transforms    = 0;
    unsigned cap           = 1;
    unsigned told_symbols  = 0;
    unsigned override_value = 0;
  };
  static batch_stats_t batch_stats();

  /// \brief Whether this run asks the open block to be handed over instead of committed (D1 step 1).
  ///
  /// \c OCUDU_DFT_RELEASE_BLOCK, **default ON** since the \c s46 controlled leg pair (design document
  /// 5.9.49): \c =0 is the one-line retreat and is what a CONTROL arm must set, since "unset" now means
  /// armed. The decision itself is grid_handover_armed()'s, so this engine, the counter line it prints, the
  /// OFDM demodulator's startup warning and the burst's trace cannot disagree about what the run asked for.
  ///
  /// Arming it also selects the BACK-END queue for the block (a command buffer belongs to the queue that
  /// created it, and the lane commits on that one - see init()), and it puts the whole release path in play:
  /// a block the receiving chain hands over is committed by whoever claims it, not by this engine.
  static bool block_release_enabled();

  /// \brief Hands the open block's command buffer over, UNCOMMITTED, to whoever reads \p grid_base (D1).
  ///
  /// The counterpart of commit_open(): the block is closed exactly the same way - the encoder is ended, so
  /// the dispatches encoded so far are complete and the adopter opens its own encoder - but the buffer is
  /// not committed. The caller becomes its submitter, which in the intended use is the lane: the buffer is
  /// deposited under the resource grid the block wrote (shared_burst::deposit_released()), the hop that
  /// reads that grid takes it by the same address, and the stages that follow (the extraction, the weights,
  /// the equalization, the demapping) are encoded into it - so the whole hop is ONE submission.
  ///
  /// \param[in] grid_base Storage base of the resource grid this block wrote
  ///            (resource_grid_device_view::base): the key its consumer takes it by. The two sides name the
  ///            same address by construction (the writer's and the reader's device views describe one
  ///            storage), which is what makes the pairing exact instead of lucky.
  ///
  /// \note What the engine stops doing, and what the caller therefore owes:
  ///  * the buffer is not committed, not counted in \c [metal_stats] \c dft \c commits, and not published
  ///    on the front-end chain - so \c wait_all() does not cover it any more;
  ///  * no front-end fence is signalled on it: whoever commits it owns its fences (the release path is one
  ///    command buffer per hop, so the order the fence exists to provide is INSIDE that buffer - the grid's
  ///    producer is its first dispatch);
  ///  * \c wait_slot() cannot be honoured for the slots it carries, and says so instead of pretending
  ///    (the caller promised that nothing on the host reads those transforms' output - that promise is the
  ///    whole point of the release);
  ///  * **a deposit nobody takes is never committed by anyone.** The caller must therefore release only
  ///    where a consumer for that grid is guaranteed (see ofdm_demodulator_impl::finish_symbol());
  ///  * ⚠ **and the transforms must not outlive their INPUT.** `finish_symbol()` returning is what tells
  ///    the receiving chain that the samples of that symbol may be recycled (puxch_processor_impl::
  ///    finish_oldest_symbol() retires the receive-buffer handle right after it), and today that is true
  ///    only because the host waits for this engine there. A released block executes LATER - at the lane's
  ///    commit - so the zero-copy radio input (`grid_write::time_samples`) would be read after the radio
  ///    has overwritten it: measured on air as `crc=KO 942 / OK 46` at `sinr=37.6 dB`, i.e. correct
  ///    samples replaced by stale ones with nothing failing. **The handover is therefore NOT called from
  ///    the receiving chain** (reverted in 2026-09-21's leg, see 5.9.7). What makes it legal again is
  ///    retain_for_block(): the input's handle travels with the block and is released when the adopted
  ///    buffer completes.
  ///
  /// \note The grid of the released block is mapped through the PROCESS-WIDE cache
  ///       (shared_queue::wrap_no_copy), not through this engine's private one, so the stages that read it
  ///       through the same cache bind the SAME \c MTLBuffer object. That is not tidiness: two objects over
  ///       one address are unordered to Metal (no barrier and no encoder boundary fixes it; see 5.9.5 and
  ///       wip/metal_alias_order.mm case G), so two objects here would be a silent wrong-data path. A grid
  ///       that cannot be mapped that way is REFUSED by submit_slot_grid_write() rather than copied.
  ///
  /// \return The command buffer as an opaque handle (an \c id&lt;MTLCommandBuffer&gt;, usable from
  ///         Objective-C++ only), or nullptr when the release path is not armed or no block is open. The
  ///         engine holds the most recently released buffer alive until it releases the next one, which is
  ///         the handle's lifetime.
  void* release_block(const void* grid_base);

  /// \brief Tells the engine which receiving slot the transforms it is about to submit belong to.
  ///
  /// Instrumentation: the GPU lane probe accounts the transforms as one group per slot (see
  /// gpu_lane_probe::register_front_end_commit()), which is what gives the front end a place in the
  /// device-side timeline the back-end lane is measured on.
  void set_lane_slot(uint64_t slot_index);

  /// \brief Tells the engine how many OFDM symbols one receiving slot of THIS cell carries (Q9-F4).
  ///
  /// 14 with a normal cyclic prefix, **12 with an extended one**, and the batched front end's unit is one
  /// slot: with the knob at its default (AUTO) the front end defers up to that many transforms into one
  /// dispatch, so the number has to come from the cell's own numerology instead of a literal 14 (user
  /// correction, dev doc 6.33). An engine that is never told does NOT batch - a slot is the unit the
  /// mechanism is defined on, and guessing its size is the assumption this rule removes.
  ///
  /// The same call site that knows the slot knows this (ofdm_demodulator_impl::set_lane_slot()), so the two
  /// travel together.
  void set_slot_symbols(unsigned nof_symbols_per_slot);

  // NOTE (5.9.65, user ruling A): the front-end fence's accessors and self-test were declared here and have
  // been retired with the mechanism (see the note in ocudu_metal_queue.h).


  /// \brief Commits the transform held in slot \c slot of the batch buffers without waiting.
  ///
  /// The input and output buffers cover max_batch() transforms; this entry point lets a pipelined
  /// caller keep several transforms in flight in different slots (a ring) and synchronize once
  /// every few submissions. \c in / \c out must be the base of the whole batch buffer.
  /// \return True when the dispatch was encoded and committed.
  bool submit_slot(const void* in, void* out, unsigned slot);

  /// \brief Waits for the transform submitted in \c slot (no-op when nothing is pending there).
  ///
  /// Unlike wait_all(), this only waits for that slot's command buffer, so a pipelined caller that
  /// keeps several transforms in flight does not stall on the newest submission.
  /// \return False when the slot's command buffer failed.
  bool wait_slot(unsigned slot);

  /// \brief Waits for every command buffer committed through the shared Metal queue.
  ///
  /// Command buffers of the shared queue complete in submission order, so this drains every
  /// previously submitted stage (DFT, channel estimator, equalizer, demapper, LDPC).
  /// \return False when a command buffer failed.
  static bool wait_all();

  /// \brief Parameters of the optional resource grid write encoded in the same command buffer as the transform.
  ///
  /// The engine is told where to write and how to compensate; which grid element corresponds to a (port, symbol) pair
  /// is the caller's business (see ocudu::dft_processor_grid_write).
  struct grid_write {
    /// Grid storage, addressable by the device (page-aligned: it is wrapped as a no-copy buffer).
    const void* grid_base = nullptr;
    /// Whole grid storage in bytes (mapped page-rounded, like every other zero-copy buffer of the engine).
    size_t grid_bytes = 0;
    /// Element offset of this (port, symbol) within the grid.
    uint32_t dst_offset = 0;
    /// Grid subcarriers to write.
    uint32_t nof_subc = 0;
    /// Rotation of the transform output: grid[i] <- transform[(i + map_offset) % size].
    uint32_t map_offset = 0;
    /// Per-symbol compensation (phase compensation times the output scaling), real and imaginary parts.
    float phase_re = 1.0F;
    float phase_im = 0.0F;
    /// Apply the per-element table published with set_grid_write_window().
    bool apply_window = false;

    /// \name Transform input read straight from the radio's int16 buffer (see the kernel's input_params).
    ///
    /// When \c time_samples is not null the transform reads its input there instead of \c in, which
    /// the caller then does not have to fill. The pointer is a SLICE of a page-aligned allocation
    /// (the RX chain's baseband buffer, see baseband_gateway_buffer_dynamic_aligned): the engine
    /// looks the allocation up in the process-wide registry and wraps THAT - one mapping for the
    /// whole buffer, reused by every symbol - passing the slice's offset to the kernel. Wrapping the
    /// per-symbol slice instead would ask the cache for a different length at every symbol, which is
    /// the re-map the demapper's wrap_length() exists to avoid.
    ///
    /// submit_slot_grid_write() returns false when the samples do not belong to a registered
    /// allocation, or when the slice (as \c time_window_start + the transform size) does not fit in
    /// it: the caller then stages its input on the host, exactly as before this existed.
    ///@{
    const void* time_samples      = nullptr;
    size_t      time_samples_bytes = 0;
    uint32_t    time_window_start  = 0;
    float       time_gain          = 1.0F;
    ///@}
  };

  /// \brief Publishes the per-element compensation table of the grid write (one complex entry per transform element).
  ///
  /// \param[in] window      Interleaved real/imaginary floats (a \c float2 per entry), or nullptr to clear the table.
  /// \param[in] nof_entries Number of complex entries of \c window.
  /// \return True on success.
  bool set_grid_write_window(const void* window, unsigned nof_entries);

  /// \brief Commits the transform held in slot \c slot together with the write of one grid symbol, without waiting.
  ///
  /// \c in and \c out are the batch buffers of submit_slot() (the caller has filled the input of that slot), and the
  /// caller must not also submit the transform through submit_slot(). The grid write is part of the transform kernel
  /// itself (its final store), so this is ONE dispatch: no second dispatch and, more importantly, no cross-dispatch
  /// dependency and therefore no memory barrier between them. The two-dispatch form measured several times slower on a
  /// pipelined slot (and a barrier is what the sync model would have required there - see the pipeline notes).
  /// \return True when the dispatch was encoded and committed.
  bool submit_slot_grid_write(const void* in, void* out, unsigned slot, const grid_write& write);

  /// GPU-side duration of the last transform in microseconds (0 when unavailable).
  double last_gpu_wait_us() const;

private:
  /// \brief Encodes and commits \c nof_transforms transforms starting at slot \c first_slot;
  /// \c wait_for_completion selects the synchronous run() path or the non-waiting submit() path.
  bool submit_at(
      const void* in, void* out, unsigned nof_transforms, unsigned first_slot, bool wait_for_completion);

  void* impl = nullptr;
};

} // namespace metal
} // namespace ocudu
