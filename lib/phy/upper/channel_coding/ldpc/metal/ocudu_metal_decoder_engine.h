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
  /// Decoder algorithm selection.
  enum class algo {
    /// SynchroPlus LLS bit-flipping heuristic (see PLAN.md 4.6).
    lls,
    /// Normalized min-sum, flooding schedule (ocudu-side kernel).
    nms,
    /// Normalized min-sum, layered schedule mirroring the CPU decoder.
    nms_layered,
  };

  /// CSR edge layout + layer tables for the layered NMS mode (built by the adapter).
  struct layered_info {
    /// Offsets into edge_vn / c2v, M_aligned + 1 entries (4KB-aligned, engine lifetime).
    const uint32_t* row_start = nullptr;
    /// VN index of each edge, no_edges entries (4KB-aligned, engine lifetime).
    const uint32_t* edge_vn = nullptr;
    /// Total number of H set bits.
    uint32_t no_edges = 0;
    /// Number of layers (base-graph check rows): 46 (BG1) / 42 (BG2).
    uint32_t n_layers = 0;
    /// Lifting size (rows per layer).
    uint32_t z = 0;
    /// Flat array of n_layers fixed-size layer descriptors (4+4+20 uint32s each).
    const uint32_t* layer_descs = nullptr;
  };

  decoder_engine()  = default;
  ~decoder_engine();

  decoder_engine(const decoder_engine&)            = delete;
  decoder_engine& operator=(const decoder_engine&) = delete;

  /// \brief One-shot setup: device/queue/pipeline states, zero-copy wrap of the host matrices.
  ///
  /// \param[in] n_logical      Codeblock length N (BG1: 68Z, BG2: 52Z).
  /// \param[in] m_logical      Number of parity checks M (BG1: 46Z, BG2: 42Z).
  /// \param[in] factor         LLS step size (alpha) or NMS normalization factor, per \c mode.
  /// \param[in] h              Packed parity-check matrix, M_aligned rows x ceil(N/32) words,
  ///                           little-endian bit order; must be 4KB-aligned and outlive this object.
  /// \param[in] ht             Packed transpose, N_aligned rows x ceil(M/32) words; same lifetime
  ///                           requirements as \c h.
  /// \param[out] col_weights_out Column weights (N_aligned entries); the engine computes them
  ///                           (LLS mode only, may be null in NMS mode).
  /// \param[in] mode           Decoder algorithm.
  /// \param[in] sat            NMS soft-bit saturation magnitude (0 = disabled; mirrors the CPU's
  ///                           promotion_sum, |soft| > 63 -> fixed bit).
  /// \param[in] layered        CSR edge layout and layer tables (nms_layered mode only, may be null).
  /// \param[in] et_enabled     nms_layered only: dispatch the per-round ET gate (GPU-internal
  ///                           early termination, single command buffer; disable for A/B).
  /// \return True on success.
  bool init(uint32_t n_logical, uint32_t m_logical, float factor, const uint32_t* h, const uint32_t* ht,
            uint32_t* col_weights_out, algo mode = algo::lls, float sat = 0.0F,
            const layered_info* layered = nullptr, bool et_enabled = true);

  /// \brief Synchronous decode of one codeblock.
  ///
  /// \param[in]  in_fp16  N_aligned fp16 LLRs, 4KB-aligned. Mutated in place (the kernels update
  ///                      the LLRs; the final values hold the hard decisions in their sign bits).
  /// \param[out] out_bits n_info bytes (0/1 hard decisions of the information bits).
  /// \param[in]  max_iter Maximum number of iterations (GPU-internal syndrome early stop applies).
  /// \param[out] error_count_out Final number of unsatisfied check equations (0 = clean syndrome).
  /// \return The number of iterations actually executed (NMS modes), or a negative value on
  ///         failure. With ET disabled the layered mode reports max_iter instead.
  int decode(const void* in_fp16, uint8_t* out_bits, int max_iter, uint32_t* error_count_out);

  /// Returns the number of information bits of the code (N - M).
  uint32_t get_n_info() const;

private:
  void* impl = nullptr;
};

} // namespace metal
} // namespace ocudu
