// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief C++ front end of the Metal soft-demapper engine (Objective-C++ implementation in
/// ocudu_demod_metal_engine.mm). One engine instance per demapper object; device/queue/
/// pipeline shared process-wide, following the equalizer engine conventions: zero-copy
/// wraps of page-aligned staging buffers with a length-checked pointer cache, one command
/// buffer per call, and the compile-time [metal_stats] probe.

#pragma once

#include <cstdint>

namespace ocudu {
namespace metal {

class demod_metal_engine
{
public:
  demod_metal_engine()  = default;
  ~demod_metal_engine();

  demod_metal_engine(const demod_metal_engine&)            = delete;
  demod_metal_engine& operator=(const demod_metal_engine&) = delete;

  /// \brief One-shot setup (shared device/queue/pipeline).
  /// \return True on success.
  bool init();

  /// \brief Synchronous soft demodulation of one symbol batch.
  /// \param[in]  symbols     Staged modulation symbols (float2), page-aligned.
  /// \param[in]  noise_var   Staged noise variances (float), page-aligned.
  /// \param[out] llrs        Demodulated LLRs (int8, [symbol][bit]), page-aligned.
  /// \param[in]  nof_symbols Number of symbols.
  /// \param[in]  mod         Kernel modulation id (0 = QPSK, 1 = 16QAM, 2 = 64QAM, 3 = 256QAM).
  /// \return True on success.
  bool demodulate(const void* symbols,
                  const void* noise_var,
                  void*       llrs,
                  unsigned    nof_symbols,
                  unsigned    mod);

  /// GPU-side duration of the last call in microseconds (0 when unavailable).
  double last_gpu_wait_us() const;

private:
  void* impl = nullptr;
};

} // namespace metal
} // namespace ocudu
