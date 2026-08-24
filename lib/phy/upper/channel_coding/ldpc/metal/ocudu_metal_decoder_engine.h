// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief ocudu-side Metal engine for the layered NMS LDPC GPU decoder.
///
/// Thin Objective-C++ implementation (see ocudu_metal_decoder_engine.mm) that loads the
/// pre-compiled .metallib shader libraries (offline xcrun metal/metallib) via
/// newLibraryWithURL and executes the layered normalized min-sum schedule: per-layer CN/VN
/// kernels chained inside one command buffer, a per-round final syndrome refresh, and a
/// GPU-internal early-termination gate. The flooding and asynchronous delta-BP variants
/// load their own pre-compiled libraries.

#pragma once

#include <cstdint>

namespace ocudu {
namespace metal {

/// Synchronous single-instance layered-NMS LDPC decoder engine, pinned to one (N, M) code size.
class decoder_engine
{
public:
  /// Decoder algorithm selection.
  enum class algo {
    /// Layered normalized min-sum (fused CN+VN kernel, ~46 dispatches/round).
    layered,
    /// Flooding normalized min-sum (2 dispatches/round, more iterations).
    flooding,
    /// Layered NMS as ONE persistent dispatch: W = min(Z, 128) resident
    /// threadgroups run the (iteration, layer) loops inside the kernel with a
    /// software grid barrier between layers (metal_persistent).
    layered_persistent,
    /// Asynchronous residual (delta) belief propagation: a BARRIER-FREE
    /// persistent grid (one threadgroup per check row plus a syndrome
    /// poller) injects per-edge residual deltas into a fixed-point atomic
    /// posterior pool (metal_async).
    async_delta,
    /// LLS (likelihood-erosion bit-flipping) decoder, restored from the git
    /// history (SynchroPlus 4-kernel shader, see PLAN.md 4.6/4.12): 2
    /// dispatches per round, extreme parallelism, ~5-8 dB weaker than the
    /// layered NMS on BLER.
    lls,
  };

  /// CSR edge layout for the layered schedule (built by the adapter). The fused
  /// CN+VN kernel needs no per-layer column tables: within one layer no two
  /// lifted rows share a variable node (3GPP base-graph property).
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
  };

  decoder_engine()  = default;
  ~decoder_engine();

  decoder_engine(const decoder_engine&)            = delete;
  decoder_engine& operator=(const decoder_engine&) = delete;

  /// \brief One-shot setup: device/queue/pipeline states, zero-copy wrap of the host matrices.
  ///
  /// \param[in] n_logical      Codeblock length N (BG1: 68Z, BG2: 52Z).
  /// \param[in] m_logical      Number of parity checks M (BG1: 46Z, BG2: 42Z).
  /// \param[in] factor         NMS normalization factor (alpha) / LLS step size, per \c mode.
  /// \param[in] beta           Offset min-sum parameter (0 = plain NMS).
  /// \param[in] h              Packed parity-check matrix, M_aligned rows x ceil(N/32) words,
  ///                           little-endian bit order; must be 4KB-aligned and outlive this object.
  /// \param[in] ht             Packed transpose, N_aligned rows x ceil(M/32) words (flooding
  ///                           and LLS modes; may be null in layered mode).
  /// \param[in] layered        CSR edge layout and layer tables (layered mode only).
  /// \param[in] mode           Decoder algorithm.
  /// \param[in] et_enabled     Dispatch the per-round ET gate (layered mode only; the flooding
  ///                           kernels always run their fused convergence check).
  /// \return True on success.
  bool init(uint32_t n_logical, uint32_t m_logical, float factor, float beta, const uint32_t* h,
            const uint32_t* ht, const layered_info& layered, algo mode = algo::layered,
            bool et_enabled = true);

  /// \brief Synchronous decode of one codeblock.
  ///
  /// \param[in]  in_fp16  Codeblock LLRs, 4KB-aligned: N_aligned fp16 LLRs in LLS mode (updated in
  ///                      place; the final values hold the hard decisions in their sign bits) or
  ///                      N_aligned int8 LLRs for the NMS modes (converted to fp16 on the GPU).
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
