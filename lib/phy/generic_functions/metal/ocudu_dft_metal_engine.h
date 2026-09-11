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
  bool run(const void* in, void* out);

  /// GPU-side duration of the last transform in microseconds (0 when unavailable).
  double last_gpu_wait_us() const;

private:
  void* impl = nullptr;
};

} // namespace metal
} // namespace ocudu
