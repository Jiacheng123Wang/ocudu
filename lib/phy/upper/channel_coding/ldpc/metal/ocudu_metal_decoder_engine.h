// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief ocudu-side Metal engine for the SynchroPlus LDPC GPU decoder.
///
/// Thin Objective-C++ implementation (see ocudu_metal_decoder_engine.mm) that mirrors the
/// SynchroPlus GPUEngineImpl contract (buffer indices, dispatch sizes, DecodeCtrl layout) but
/// loads the shader either from an offline-compiled metallib (when the Xcode toolchain built
/// one) or from the .metal source embedded at configure time and compiled at runtime with
/// newLibraryWithSource. This keeps the verbatim SynchroPlus copies untouched.

#pragma once

#include <cstdint>

namespace ocudu {
namespace metal {

/// Synchronous single-instance LDPC decoder engine, pinned to one (N, M) code size.
class decoder_engine
{
public:
  decoder_engine()  = default;
  ~decoder_engine();

  decoder_engine(const decoder_engine&)            = delete;
  decoder_engine& operator=(const decoder_engine&) = delete;

  /// \brief One-shot setup: device/queue/pipeline states, zero-copy wrap of the host matrices.
  ///
  /// \param[in] n_logical      Codeblock length N (BG1: 68Z, BG2: 52Z).
  /// \param[in] m_logical      Number of parity checks M (BG1: 46Z, BG2: 42Z).
  /// \param[in] alpha          LLS update step size (reference value 0.45).
  /// \param[in] h              Packed parity-check matrix, M_aligned rows x ceil(N/32) words,
  ///                           little-endian bit order; must be 4KB-aligned and outlive this object.
  /// \param[in] ht             Packed transpose, N_aligned rows x ceil(M/32) words; same lifetime
  ///                           requirements as \c h.
  /// \param[out] col_weights_out Column weights (N_aligned entries); the engine computes them.
  /// \return True on success.
  bool init(uint32_t n_logical, uint32_t m_logical, float alpha, const uint32_t* h, const uint32_t* ht,
            uint32_t* col_weights_out);

  /// \brief Synchronous decode of one codeblock.
  ///
  /// \param[in]  in_fp16  N_aligned fp16 LLRs, 4KB-aligned. Mutated in place (the kernels update
  ///                      the LLRs; the final values hold the hard decisions in their sign bits).
  /// \param[out] out_bits n_info bytes (0/1 hard decisions of the information bits).
  /// \param[in]  max_iter Maximum number of iterations (GPU-internal syndrome early stop applies).
  /// \param[out] error_count_out Final number of unsatisfied check equations (0 = clean syndrome).
  /// \return The number of iterations actually executed, or a negative value on failure.
  int decode(const void* in_fp16, uint8_t* out_bits, int max_iter, uint32_t* error_count_out);

  /// Returns the number of information bits of the code (N - M).
  uint32_t get_n_info() const;

private:
  void* impl = nullptr;
};

} // namespace metal
} // namespace ocudu
