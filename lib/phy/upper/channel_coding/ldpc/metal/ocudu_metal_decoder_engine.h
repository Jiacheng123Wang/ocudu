// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief ocudu-side Metal engine for the layered NMS LDPC GPU decoder.
///
/// Thin Objective-C++ implementation (see ocudu_metal_decoder_engine.mm) that compiles the
/// shader at runtime from the .metal source embedded at configure time (no Xcode needed)
/// and executes the layered normalized min-sum schedule: per-layer CN/VN kernels chained
/// inside one command buffer, a per-round final syndrome refresh, and a GPU-internal
/// early-termination gate.

#pragma once

#include <cstdint>

namespace ocudu {
namespace metal {

/// Synchronous single-instance layered-NMS LDPC decoder engine, pinned to one (N, M) code size.
class decoder_engine
{
public:
  /// CSR edge layout + layer tables for the layered schedule (built by the adapter).
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
  /// \param[in] factor         NMS normalization factor (alpha).
  /// \param[in] beta           Offset min-sum parameter (0 = plain NMS).
  /// \param[in] h              Packed parity-check matrix, M_aligned rows x ceil(N/32) words,
  ///                           little-endian bit order; must be 4KB-aligned and outlive this object.
  /// \param[in] layered        CSR edge layout and layer tables (required).
  /// \param[in] et_enabled     Dispatch the per-round ET gate (GPU-internal early termination,
  ///                           single command buffer; disable for A/B).
  /// \return True on success.
  bool init(uint32_t n_logical, uint32_t m_logical, float factor, float beta, const uint32_t* h,
            const layered_info& layered, bool et_enabled = true);

  /// \brief Synchronous decode of one codeblock.
  ///
  /// \param[in]  in_fp16  N_aligned fp16 LLRs, 4KB-aligned. Mutated in place (the kernels update
  ///                      the LLRs; the final values hold the hard decisions in their sign bits).
  /// \param[out] out_bits n_info bytes (0/1 hard decisions of the information bits).
  /// \param[in]  max_iter Maximum number of iterations (the GPU-internal ET gate applies).
  /// \param[out] error_count_out Final number of unsatisfied check equations (0 = clean syndrome).
  /// \return The number of iterations actually executed (ET enabled), max_iter when ET is
  ///         disabled, or a negative value on failure.
  int decode(const void* in_fp16, uint8_t* out_bits, int max_iter, uint32_t* error_count_out);

  /// Returns the number of information bits of the code (N - M).
  uint32_t get_n_info() const;

  /// GPU-side duration of the last decode in microseconds (0 when unavailable).
  /// Wall time minus this is the CPU-side fixed overhead (LLR pack, zero-copy
  /// wrap, command recording, readback).
  double last_gpu_wait_us() const;

private:
  void* impl = nullptr;
};

} // namespace metal
} // namespace ocudu
