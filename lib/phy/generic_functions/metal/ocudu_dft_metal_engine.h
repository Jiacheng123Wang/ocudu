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
