// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief ocudu-side Metal engine for the 2D MMSE channel estimator.
///
/// Thin Objective-C++ implementation (see ocudu_metal_mmse_engine.mm) that loads the
/// pre-compiled .metallib shader libraries and executes the two MMSE core operators:
///   K1  ocudu_mmse_inv.metal   : batched Gauss-Jordan inversion of LxL matrices
///                                (one threadgroup per system).
///   K2  ocudu_mmse_apply.metal : batched W . y block matrix multiplications
///                                (one threadgroup per (system, block)).
/// The engine follows the LDPC Metal engine pattern: zero-copy buffers
/// (newBufferWithBytesNoCopy, 4KB alignment), synchronous waitUntilCompleted and
/// last_gpu_wait_us() GPU-side timing.

#pragma once

#include <cstdint>

namespace ocudu {
namespace metal {

/// Synchronous single-instance MMSE compute engine.
class mmse_engine
{
public:
  /// Maximum number of systems per slot (ports x layers x hops).
  static constexpr unsigned MAX_SYSTEMS = 8;

  mmse_engine()  = default;
  ~mmse_engine();

  mmse_engine(const mmse_engine&)            = delete;
  mmse_engine& operator=(const mmse_engine&) = delete;

  /// \brief One-shot setup: device/queue/pipelines and .metallib loading.
  ///
  /// \param[in] metallib_path Path to the pre-compiled shader library (may be null to use
  ///                          the baked-in default path).
  /// \return True on success.
  bool init(const char* metallib_path = nullptr);

  /// \brief Batched inversion (K1): A_inv = (A)^-1 for each system, in-place Gauss-Jordan.
  ///
  /// \param[in,out] a           [systems][n][n] row-major matrices (overwritten with the inverse).
  /// \param[in]     n           Matrix order (<= 36).
  /// \param[in]     nof_systems Number of systems (<= MAX_SYSTEMS).
  /// \return True on success.
  bool invert(float* a, unsigned n, unsigned nof_systems);

  /// \brief Batched block matrix multiplication (K2): h = W . y for each (system, block).
  ///
  /// \param[in]  w           [systems][nout][L] row-major real weights.
  /// \param[in]  y           [systems][nof_blocks][L] real/imag interleaved pilot vectors.
  /// \param[out] h           [systems][nof_blocks][nout] real/imag interleaved outputs.
  /// \param[in]  nout        Number of block output positions (<= 504).
  /// \param[in]  L           Number of block pilots (<= 36).
  /// \param[in]  nof_systems Number of systems (<= MAX_SYSTEMS).
  /// \param[in]  nof_blocks  Number of time-frequency blocks.
  /// \return True on success.
  bool apply(const float* w, const float* y, float* h, unsigned nout, unsigned L, unsigned nof_systems,
             unsigned nof_blocks);

  /// \brief Combined K1 + K1b + K2 pipeline in ONE command buffer (single commit/wait):
  /// A^-1 (in place), W = R_hp . A^-1, h = W . y. This is the per-slot hot path.
  ///
  /// \param[in,out] a           [systems][L][L] matrices (overwritten with the inverse).
  /// \param[in]     r_hp        [systems][nout][L] row-major cross-correlation matrices.
  /// \param[out]    w           [systems][nout][L] weight matrices.
  /// \param[in]     y           [systems][nof_blocks][2L] real/imag interleaved pilots.
  /// \param[out]    h           [systems][nof_blocks][2nout] real/imag interleaved outputs.
  /// \param[in]     nout        Number of block output positions (<= 504).
  /// \param[in]     L           Number of block pilots (<= 36).
  /// \param[in]     nof_systems Number of systems (<= MAX_SYSTEMS).
  /// \param[in]     nof_blocks  Number of time-frequency blocks.
  /// \return True on success.
  bool run(float* a, const float* r_hp, float* w, const float* y, float* h, unsigned nout, unsigned L,
           unsigned nof_systems, unsigned nof_blocks);

  /// \brief Hot path (v1): K1b + K2 in ONE command buffer with a single commit/wait.
  /// The A^-1 inversion runs on the CPU (the batched 36x36 Gauss-Jordan kernel is
  /// barrier-bound on the GPU - see PLAN.md 7.0.6); the Metal inversion kernel remains
  /// as the algorithm skeleton and the golden reference.
  bool run_weights_only(const float* a_inv, const float* r_hp, float* w, const float* y, float* h, unsigned nout,
                        unsigned L, unsigned nof_systems, unsigned nof_blocks);

  /// Returns the GPU-side duration of the last operation in microseconds (0 when unavailable).
  double last_gpu_wait_us() const;

private:
  void* impl = nullptr;
};

} // namespace metal
} // namespace ocudu
