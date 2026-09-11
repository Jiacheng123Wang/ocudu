// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief 2D time-frequency block MMSE channel estimator (Metal-skeleton adapter, v1 CPU math).
///
/// Implements W = R_hp (R_pp + sigma2 I)^-1 over time-frequency blocks (default 3 PRB x 14
/// symbols), with the separable correlation model R = R_t (x) R_f:
///   R_f(delta_f) = 1 / (1 + (2 pi delta_f tau_rms)^2)   (exponential PDP, real part)
///   R_t(delta_t) = J0(2 pi f_d delta_t)                 (Jakes Doppler)
/// The statistics come from the channel_statistics_estimator interface: v1 uses the existing
/// classical noise estimation for sigma2 and FIXED configurable constants for tau_rms and f_d.
/// The matrix inversion and the block matrix multiplications are the MMSE core operators that
/// will run as Metal kernels (K1/K2); the v1 adapter executes them on the CPU behind the same
/// engine-shaped interface so the Metal engine can drop in without touching the algorithm.

#pragma once

#include "../port_channel_estimator_average_impl.h"
#include "channel_statistics_estimator.h"
#include "ocudu_metal_mmse_engine.h"
#include "ocudu/ocudulog/ocudulog.h"

namespace ocudu {

/// 2D block MMSE port channel estimator (Apple Silicon Metal skeleton, CPU reference math).
class port_channel_estimator_metal_mmse_impl : public port_channel_estimator_average_impl
{
public:
  /// Maximum block size: 3 PRB x 14 symbols = 504 positions.
  /// Maximum block pilots: 3 PRB x 6 RE (type 1) x 4 DM-RS symbols (PUSCH pos2 + 3 additional
  /// positions - the E2E cell uses {2,7,11}, i.e. 3 symbols, see PLAN.md 7.0.9).
  static constexpr unsigned MAX_BLOCK_PRB      = 3;
  static constexpr unsigned MAX_BLOCK_OUT      = MAX_BLOCK_PRB * NOF_SUBCARRIERS_PER_RB * MAX_NSYMB_PER_SLOT;
  static constexpr unsigned MAX_DMRS_SYMBOLS   = 4;
  static constexpr unsigned MAX_BLOCK_PILOTS   = MAX_BLOCK_PRB * 6 * MAX_DMRS_SYMBOLS;

  /// \brief Constructor.
  /// \param[in] interp          Interpolator (unused by the MMSE path; kept for the base class).
  /// \param[in] ta_estimator    Time alignment estimator (reused from the base class).
  /// \param[in] stats_estimator Channel statistics provider (v1: fixed constants).
  /// \param[in] block_prb       Time-frequency block size in PRBs (1..3).
  /// \param[in] compensate_cfo  Whether CFO compensation is active.
  /// \param[in] use_matrix_engine Whether to run the K1b/K2 work on the simdgroup_matrix
  ///                            8x8 pipelines (metal_nn_mmse A/B twin); the statistics,
  ///                            correlation math and CPU inversion are identical to the
  ///                            legacy kernel path, so only the GPU kernels differ.
  port_channel_estimator_metal_mmse_impl(std::unique_ptr<interpolator>                        interp,
                                         std::unique_ptr<time_alignment_estimator>            ta_estimator_,
                                         std::shared_ptr<const channel_statistics_estimator>  stats_estimator_,
                                         unsigned                                             block_prb_,
                                         bool                                                 compensate_cfo_ = true,
                                         bool                                                 use_matrix_engine_ = false);

  /// Destructor (releases the aligned GPU staging buffers).
  ~port_channel_estimator_metal_mmse_impl() override;

  /// \brief In-place Gauss-Jordan inversion of an n x n real matrix with partial pivoting
  /// (row-major [A | I] layout, 2n columns). Returns false when the matrix is singular.
  static bool gauss_jordan_invert(span<float> a, unsigned n);

  /// Returns the GPU-side duration of the last Metal engine operation in microseconds
  /// (0 when the engine is unavailable).
  double last_gpu_wait_us() const { return engine ? engine->last_gpu_wait_us() : 0.0; }

  /// Returns whether the simdgroup 8x8 (metal_nn_mmse) pipelines are compiled and the
  /// estimator runs them on the standard blocks (dims are zero-padded to 8-alignment
  /// automatically; A/B observability: nn=0 in [mmse_time] then only means the engine
  /// itself was unavailable, never a dims fallback).
  bool matrix_accel_ready() const { return use_matrix_engine && matrix_ready; }

  /// Whether the LAST processed fd/td estimation stage actually ran the matrix kernels.
  bool nn_engaged_last() const { return last_stage_nn; }

private:
  // See the base class documentation.
  void apply_fd_td_estimation_stage(fd_td_estimation_stage_args& args) override;

  // See the base class documentation.
  void get_symbol_ch_estimate(span<cbf16_t> symbol, unsigned i_symbol, unsigned tx_layer) const override;

  // See the base class documentation.
  void get_symbol_ch_estimate(span<cbf16_t>                              symbol,
                              unsigned                                   i_symbol,
                              unsigned                                   tx_layer,
                              const bounded_bitset<MAX_NOF_SUBCARRIERS>& re_mask) const override;

  /// \brief Estimates sigma2 for the current hop reusing the existing classical noise estimator
  /// (RC smoothing of the LSE pilots followed by estimate_noise).
  float estimate_sigma2(const fd_td_estimation_stage_args& args);

  /// \brief Builds the correlation matrices of one block:
  /// \c a_out = R_pp + sigma2 I + ridge I (LxL) and \c r_hp_out = R_hp (nout x L).
  /// Sets \c nout and \c L. The MMSE weights are W = R_hp . A^-1.
  static void build_correlation_matrices(const channel_statistics&                     stats,
                                         const bounded_bitset<NOF_SUBCARRIERS_PER_RB>& re_pattern,
                                         unsigned                                       n_prb,
                                         span<const unsigned>                           dmrs_slot_symbols,
                                         unsigned                                       scs_khz,
                                         span<float>                                    a_out,
                                         span<float>                                    r_hp_out,
                                         unsigned&                                      nout,
                                         unsigned&                                      L);

  /// \brief Runs one GPU batch over n_blk equal-width (b_prb PRB) blocks on the engine and
  /// unpacks the estimates into the grid. Covers BOTH the standard blocks and the tail block
  /// (or a whole hop narrower than block_prb): with the engine ready, no hop block ever runs
  /// the CPU reference math. The caller must have filled w_r_pp / w_r_hp via
  /// build_correlation_matrices(b_prb ...) beforehand; the pilot view must stay alive.
  ///
  /// \param matrix Selects the metal_nn_mmse simdgroup path (zero-padded staging + run_nn)
  ///               vs the legacy kernels (run_weights_only).
  /// \param npt    Number of DM-RS symbols of the hop (the block pilot count is
  ///               L = npt x b_prb x 6 for the type-1 comb-2 pattern used here).
  /// \return True when the engine processed and unpacked the batch; false when the engine call
  ///         failed (wrap/commit error), in which case the caller MUST fall back to the CPU
  ///         reference math for these blocks (S-1 audit fix: the return value was previously
  ///         ignored, which could silently leave stale channel estimates in the grid).
  bool run_engine_blocks(const fd_td_estimation_stage_args& args,
                         unsigned                           gb_start,
                         unsigned                           n_blk,
                         unsigned                           b_prb,
                         unsigned                           nout,
                         unsigned                           L,
                         unsigned                           npt,
                         bool                               matrix);

  /// Metal compute engine (K1 batched inversion + K2 batched block matmul); the CPU reference
  /// math remains as the automatic fallback when the engine is unavailable or fails.
  std::unique_ptr<ocudu::metal::mmse_engine> engine;
  bool                                       engine_ready = false;

  /// metal_nn_mmse (matrix-accelerated) flavor: the engine also compiles the simdgroup 8x8
  /// kernels and the standard-block path ALWAYS runs them - dims that are not multiples of 8
  /// are zero-padded to ceil8(nout)/ceil8(L) by the packing code (see ocudu_mmse_*_matrix.metal
  /// for the padding contract). nn=0 can therefore only mean the matrix engine itself is
  /// unavailable (stale metallib, NOGPU); the CPU path remains the fallback for that case.
  bool use_matrix_engine = false;
  bool matrix_ready      = false;

  /// Whether the last fd/td estimation stage engaged the simdgroup 8x8 kernels
  /// (diagnostics/A-B observability; see nn_engaged_last()).
  bool last_stage_nn = false;

  /// Maximum number of full blocks per slot for the configured block size.
  unsigned max_blocks;

  /// GPU staging buffers (4KB aligned, engine lifetime):
  /// a_slots [MAX_LAYERS][36][36], w_slots [MAX_LAYERS][504][36],
  /// y_slots [MAX_LAYERS][max_blocks][2*36], h_slots [MAX_LAYERS][max_blocks][2*504],
  /// qy_slots (metal_nn_mmse only) [MAX_LAYERS][ceil(max_blocks/4)][36][8]: quad-packed
  /// real pilot matrix for the simdgroup 8x8 apply kernel.
  float* gpu_a = nullptr;
  float* gpu_r_hp = nullptr;
  float* gpu_w = nullptr;
  float* gpu_y = nullptr;
  float* gpu_qy = nullptr;
  float* gpu_h = nullptr;

  /// Statistics provider (fixed constants in v1).
  std::shared_ptr<const channel_statistics_estimator> stats_estimator;

  /// Time-frequency block size in PRBs.
  unsigned block_prb;

  /// Estimated full time-frequency grid: slice (layer * MAX_NSYMB_PER_SLOT + symbol), one slice
  /// per layer and OFDM symbol of the slot, each of width 12 * nof_prb (hop RB-major order).
  static_re_buffer<MAX_LAYERS * MAX_NSYMB_PER_SLOT, MAX_NOF_SUBCARRIERS> grid_est;

  /// Auxiliary enlarged LSE buffers for the sigma2 estimation (RC smoothing).
  static_re_measurement<cf_t, MAX_NOF_PILOTS_SYMBOL, MAX_NOF_DMRS_SYMBOLS, MAX_LAYERS> tmp_lse_enlarged;
  static_re_measurement<cf_t, MAX_NOF_PILOTS_SYMBOL, MAX_NOF_DMRS_SYMBOLS, MAX_LAYERS> tmp_filtered_enlarged;

  /// Symbol-major concatenation of the LSE pilots (input to the statistics provider).
  std::array<cf_t, MAX_NOF_DMRS_SYMBOLS * MAX_NOF_PILOTS_SYMBOL> stats_pilots;

  /// Weight-matrix workspace (R_hp, R_pp + sigma2 I and the inverse, W).
  std::array<float, MAX_BLOCK_OUT * MAX_BLOCK_PILOTS>    w_r_hp;
  std::array<float, MAX_BLOCK_PILOTS * MAX_BLOCK_PILOTS> w_r_pp;
  std::array<float, MAX_BLOCK_PILOTS * MAX_BLOCK_PILOTS> w_a_inv;
  std::array<float, MAX_BLOCK_OUT * MAX_BLOCK_PILOTS>    w_mat;

  /// Block pilot vector (real/imag interleaved) and block output vector.
  std::array<float, 2 * MAX_BLOCK_PILOTS> y_block;
  std::array<float, 2 * MAX_BLOCK_OUT>    h_block;

  /// PHY log channel (debug-level diagnostics: engine status, per-phase timing).
  ocudulog::basic_logger& logger = ocudulog::fetch_basic_logger("PHY");
};

} // namespace ocudu
