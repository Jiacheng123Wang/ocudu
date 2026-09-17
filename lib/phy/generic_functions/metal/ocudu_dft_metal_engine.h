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

  /// \brief Tells the engine which receiving slot the transforms it is about to submit belong to.
  ///
  /// Instrumentation: the GPU lane probe accounts the transforms as one group per slot (see
  /// gpu_lane_probe::register_front_end_commit()), which is what gives the front end a place in the
  /// device-side timeline the back-end lane is measured on.
  void set_lane_slot(uint64_t slot_index);

  /// \brief Front-end fence: the generation the front end has committed (see shared_queue::front_end_*()).
  ///
  /// The mechanism lives in the shared command queue, whose header is Objective-C++ only; these are the
  /// C++-visible window onto it, for the unit test and the diagnostics.
  static uint64_t fence_generation();
  static uint64_t fence_nof_signals();
  static uint64_t fence_nof_waits();
  static uint64_t fence_nof_skipped_waits();

  /// \brief Self-test of the fence plumbing, for builds and tests without the receiving chain.
  ///
  /// Opens a back-end command buffer, lets the fence encode its wait on it (nothing to wait for before
  /// the first front-end commit, which is the path the offline tools take), puts one blit in it, and
  /// commits and waits for it: a command buffer that carries a wait must still complete, which is the
  /// one failure mode that would take the whole chain down instead of producing a wrong number.
  /// \param[out] waited  Whether a wait was encoded (false when nothing had been committed yet).
  /// \return True when the command buffer completed.
  static bool fence_selftest(bool& waited);

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
