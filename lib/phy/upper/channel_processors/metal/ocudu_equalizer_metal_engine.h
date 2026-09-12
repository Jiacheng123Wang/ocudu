// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief C++ front end of the Metal channel equalizer engine (Objective-C++
/// implementation in ocudu_equalizer_metal_engine.mm). One engine instance per equalizer
/// object; device/queue/pipeline shared process-wide, following the DFT/LDPC engine
/// conventions: zero-copy wraps of page-aligned staging buffers with a length-checked
/// pointer cache, one command buffer per call, and the compile-time [metal_stats] probe.

#pragma once

#include <cstdint>

namespace ocudu {
namespace metal {

class equalizer_metal_engine
{
public:
  /// Maximum dimensions supported by the kernel.
  static constexpr unsigned max_ports  = 8;
  static constexpr unsigned max_layers = 4;

  equalizer_metal_engine()  = default;
  ~equalizer_metal_engine();

  equalizer_metal_engine(const equalizer_metal_engine&)            = delete;
  equalizer_metal_engine& operator=(const equalizer_metal_engine&) = delete;

  /// \brief One-shot setup (shared device/queue/pipeline + warm-up dispatch).
  /// \return True on success.
  bool init();

  /// \brief Synchronous equalization of one symbol batch.
  /// \param[in]  h          Staged channel estimates, layout [port][layer][re], cbf16,
  ///                        page-aligned. tx_scaling is applied by the caller on the
  ///                        multi-layer path and left out on the single-layer path.
  /// \param[in]  y          Staged received symbols, layout [port][re], cbf16, page-aligned.
  /// \param[in]  sigma2     Staged per-port noise variances, float, page-aligned (used by
  ///                        the single-layer path only).
  /// \param[out] eq         Equalized symbols, layout [re][layer] interleaved, float2,
  ///                        page-aligned.
  /// \param[out] nv         Post-equalization noise variances, layout [re][layer], float,
  ///                        page-aligned.
  /// \param[in]  nof_re     Number of resource elements.
  /// \param[in]  nof_ports  Receive ports (1..8; 2/4/8 on the multi-layer path).
  /// \param[in]  nof_layers Transmit layers (1..4, <= nof_ports).
  /// \param[in]  mmse       True for the MMSE algorithm (false = ZF).
  /// \param[in]  noise_var  Noise variance estimate (max across ports, multi-layer path).
  /// \param[in]  tx_scaling Transmission gain scaling factor (single-layer path).
  /// \param[in]  h_scaling  Channel estimate scaling applied in-kernel (multi-layer path;
  ///                        use 1 on the single-layer path).
  /// \return True on success.
  bool equalize(const void* h,
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
                float       h_scaling);

  /// \brief Batched enqueue API (S2 groundwork): every dispatch enqueued between begin_batch()
  /// and flush_batch() shares a single command buffer, so a symbol batch pays one commit/wait
  /// round trip instead of one per dispatch. The caller owns the buffers: they must stay alive
  /// and untouched until flush_batch() returns.
  /// \return True on success.
  bool begin_batch();
  bool enqueue(const void* h,
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
               float       h_scaling);
  bool flush_batch();

  /// \brief Commits the batch without waiting (pairs with wait_committed()).
  ///
  /// Command buffers of one queue complete in submission order, so a single
  /// wait_committed() at the end of a burst covers every commit_batch() issued before it.
  bool commit_batch();

  /// \brief Waits for the command buffers committed so far (no-op when none are pending).
  bool wait_committed();

  /// Number of dispatches enqueued in the batch in progress (diagnostics).
  unsigned batch_size() const;

  /// True when every buffer of the last dispatch was wrapped without a copy. False means the
  /// platform refused the no-copy wrap and the engine staged through an owned MTLBuffer.
  bool last_call_used_no_copy() const;

  /// GPU-side duration of the last call in microseconds (0 when unavailable).
  double last_gpu_wait_us() const;

private:
  void* impl = nullptr;
};

} // namespace metal
} // namespace ocudu
