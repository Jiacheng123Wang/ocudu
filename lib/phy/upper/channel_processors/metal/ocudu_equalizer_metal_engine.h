// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief C++ front end of the Metal MIMO channel equalizer engine (Objective-C++
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
  /// \param[in]  h         Staged channel estimates, layout [port][layer][re], float2,
  ///                       page-aligned, tx_scaling already applied by the caller.
  /// \param[in]  y         Staged received symbols, layout [port][re], float2, page-aligned.
  /// \param[out] eq        Equalized symbols, layout [re][layer] interleaved, float2,
  ///                       page-aligned.
  /// \param[out] nv        Post-equalization noise variances, layout [re][layer], float,
  ///                       page-aligned.
  /// \param[in]  nof_re    Number of resource elements.
  /// \param[in]  nof_ports Receive ports (2, 4 or 8).
  /// \param[in]  nof_layers Transmit layers (2..4, <= nof_ports).
  /// \param[in]  mmse      True for the MMSE algorithm (false = ZF).
  /// \param[in]  noise_var Noise variance estimate (the max across ports, CPU convention).
  /// \return True on success.
  bool equalize(const void* h,
                const void* y,
                void*       eq,
                void*       nv,
                unsigned    nof_re,
                unsigned    nof_ports,
                unsigned    nof_layers,
                bool        mmse,
                float       noise_var);

  /// GPU-side duration of the last call in microseconds (0 when unavailable).
  double last_gpu_wait_us() const;

private:
  void* impl = nullptr;
};

} // namespace metal
} // namespace ocudu
