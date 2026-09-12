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
  /// \param[in] force_cpu_path  Test-only hook: skips the Metal engine entirely so the
  ///                            CPU reference loop handles every block (used by the
  ///                            head-to-head benchmark; production code leaves it false
  ///                            and selects the CPU estimator via the factory instead).
  port_channel_estimator_metal_mmse_impl(std::unique_ptr<interpolator>                        interp,
                                         std::unique_ptr<time_alignment_estimator>            ta_estimator_,
                                         std::shared_ptr<const channel_statistics_estimator>  stats_estimator_,
                                         unsigned                                             block_prb_,
                                         bool                                                 compensate_cfo_ = true,
                                         bool                                                 use_matrix_engine_ = false,
                                         bool                                                 force_cpu_path = false);

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

  /// Whether the LAST processed fd/td estimation stage merged the tail block into the standard
  /// batch (one engine call per hop instead of two). A hop without an edge block, or with more
  /// layers than the merged batch fits, never engages it, so this is what tells an A/B apart.
  bool merged_batch_last() const { return last_stage_merged; }

  /// Whether the LAST hop produced the device-side (K3) per-symbol estimates the equalizer
  /// consumes. False when the A/B knob is off, when the engine call did not cover the whole
  /// allocation, or when the engine failed.
  bool device_estimates_ready_last() const { return gpu_ce_ready; }

  /// Device-side per-symbol channel estimates of the last hop: [layer][total_re] complex cbf16,
  /// laid out per symbol by device_estimate_offsets(). Only valid when
  /// device_estimates_ready_last(); \c i_layer must be below device_estimate_layers().
  const cbf16_t* device_estimate_layer(unsigned i_layer) const
  {
    return reinterpret_cast<const cbf16_t*>(gpu_ce) + static_cast<std::size_t>(i_layer) * gpu_ce_total_re;
  }

  /// Offsets of the device estimates: symbol s occupies [offsets()[s], offsets()[s + 1]) of each
  /// layer, MAX_NSYMB_PER_SLOT + 1 entries.
  span<const unsigned> device_estimate_offsets() const
  {
    return span<const unsigned>(re_offsets.data(), MAX_NSYMB_PER_SLOT + 1);
  }

  /// Number of layers the device estimates of the last hop cover.
  unsigned device_estimate_layers() const { return gpu_ce_layers; }

private:
  // See the base class documentation.
  void apply_fd_td_estimation_stage(fd_td_estimation_stage_args& args) override;

  // See the base class documentation.
  std::optional<ch_est_device_view> get_device_ch_estimates(unsigned i_symbol, unsigned tx_layer) const override;

  // See the base class documentation.
  void get_symbol_ch_estimate(span<cbf16_t> symbol, unsigned i_symbol, unsigned tx_layer) const override;

  // See the base class documentation.
  void get_symbol_ch_estimate(span<cbf16_t>                              symbol,
                              unsigned                                   i_symbol,
                              unsigned                                   tx_layer,
                              const bounded_bitset<MAX_NOF_SUBCARRIERS>& re_mask) const override;

  /// \brief Estimates sigma2 for the current hop reusing the existing classical noise estimator
  /// (RC smoothing of the LSE pilots followed by estimate_noise).
  /// \note Expects the LSE pilots already scaled by 1 / beta (the received domain is recovered
  /// inside from \c beta_scaling), and returns the noise variance in the received domain.
  float estimate_sigma2(const fd_td_estimation_stage_args& args);

  /// \brief Builds the correlation matrices of one block:
  /// \c a_out = R_pp + sigma2 I + ridge I (LxL) and \c r_hp_out = R_hp (nout x L).
  ///
  /// \c stats.sigma2 must be the noise-to-pilot-power RATIO, as the model is unit-normalized.
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

  /// Destination strides of one staged engine batch, in elements. The block geometry of a group
  /// (nout / L, from build_correlation_matrices) may be smaller than its slots: the merged
  /// standard + tail batch gives the tail block the standard strides and pads its matrix to a
  /// block diagonal, so one engine call covers both geometries.
  struct engine_strides {
    unsigned L     = 0; ///< Pilot-count stride (Ls) of the slots.
    unsigned nout  = 0; ///< Block-output stride (Ns) of the slots.
    unsigned n_blk = 0; ///< Blocks per system in this batch.
  };

  /// \brief Stages one group of layers (systems [sys_offset, sys_offset + nof_layers)) into the
  /// engine slots: A (or A^-1 when \c gpu_invert is false), R_hp and the pilot vectors of the
  /// blocks [gb_start, gb_start + n_blk). The caller must have filled w_r_pp / w_r_hp via
  /// build_correlation_matrices(b_prb ...) beforehand and must keep the pilot view alive.
  ///
  /// Oversized slots (Ls > L) hold A padded to blockdiag(A, I) when K1 inverts it in place, and
  /// R_hp padded with zero rows/columns, so the pad columns of W come out exactly zero and the
  /// real entries keep the values of the unpadded system.
  ///
  /// \param matrix Selects the quad-packed pilot matrix (run_nn staging) over the per-block
  ///               real/imag interleaved vectors (legacy kernels).
  /// \param npt    Number of DM-RS symbols of the hop (the block pilot count is
  ///               L = npt x b_prb x 6 for the type-1 comb-2 pattern used here).
  void stage_engine_group(const fd_td_estimation_stage_args& args,
                          unsigned                           gb_start,
                          unsigned                           n_blk,
                          unsigned                           b_prb,
                          unsigned                           npt,
                          unsigned                           nout,
                          unsigned                           L,
                          unsigned                           sys_offset,
                          const engine_strides&              st,
                          bool                               matrix,
                          bool                               gpu_invert);

  /// \brief One engine call (one command buffer, one commit/wait) over the staged slots, with the
  /// optional K3 reformat stage appended to the same command buffer.
  /// \param reformat Per-symbol mask/offsets and destination of the equalizer's estimates, or
  ///                 nullptr to skip K3. Only meaningful for the legacy (non-matrix) kernels.
  /// \return False when the engine call failed (wrap/commit error), in which case the caller MUST
  ///         fall back to the CPU reference math for these blocks (S-1 audit fix: the return value
  ///         was previously ignored, which could silently leave stale channel estimates in the
  ///         grid).
  bool engine_run(unsigned                                 nout,
                  unsigned                                 L,
                  unsigned                                 nof_systems,
                  unsigned                                 nof_blocks,
                  bool                                     matrix,
                  bool                                     gpu_invert,
                  const metal::mmse_engine::reformat_stage* reformat = nullptr);

  /// \brief Unpacks the engine outputs of the group staged at \c sys_offset into the grid
  /// (symbol-major within each block; the blocks start at PRB gb_start).
  void unpack_engine_group(unsigned              gb_start,
                           unsigned              n_blk,
                           unsigned              b_prb,
                           unsigned              nout,
                           unsigned              nof_layers,
                           unsigned              sys_offset,
                           const engine_strides& st);

  /// \brief Runs one GPU batch over n_blk equal-width (b_prb PRB) blocks on the engine and
  /// unpacks the estimates into the grid - the staging, the call and the unpack of a single-group
  /// batch. Covers the standard blocks, the tail block and a whole hop narrower than block_prb:
  /// with the engine ready, no hop block ever runs the CPU reference math.
  /// \return True when the engine processed and unpacked the batch; false when the engine call
  ///         failed, in which case the caller MUST fall back to the CPU reference math.
  bool run_engine_blocks(const fd_td_estimation_stage_args& args,
                         unsigned                           gb_start,
                         unsigned                           n_blk,
                         unsigned                           b_prb,
                         unsigned                           nout,
                         unsigned                           L,
                         unsigned                           npt,
                         bool                               matrix,
                         const metal::mmse_engine::reformat_stage* reformat = nullptr);

  /// \brief Builds the per-symbol data-RE masks of the current hop - the layout the equalizer
  /// indexes its channel estimates by - and their prefix RE counts.
  ///
  /// Mirrors what the PUSCH demodulator does when it extracts the channel estimates
  /// (pusch_demodulator_impl::demodulate): the hop's allocated PRBs expanded to subcarriers, with
  /// the DM-RS REs of a DM-RS symbol removed, in ascending subcarrier order. The DM-RS RE set of a
  /// PRB is the union over the layers' RE patterns (for the type-1 pattern these are the per-layer
  /// combs, so the union is the comb set the demodulator's CDM group count selects).
  ///
  /// \param[in] nof_prb   Number of PRBs of the hop.
  /// \param[in] first_prb First PRB of the hop (the mask is relative to it, as the demodulator's
  ///                      slice of the RE mask is).
  /// \return Number of compressed REs (the destination length per layer), 0 when the hop does not
  ///         fit the mask buffers.
  unsigned stage_re_masks(const fd_td_estimation_stage_args& args, unsigned nof_prb, unsigned first_prb);

  /// Metal compute engine (K1 batched inversion + K2 batched block matmul); the CPU reference
  /// math remains as the automatic fallback when the engine is unavailable or fails.
  std::unique_ptr<ocudu::metal::mmse_engine> engine;
  bool                                       engine_ready = false;

  /// metal_nn_mmse (matrix-accelerated) flavor: the engine also compiles the simdgroup 8x8
  /// kernels and the standard-block path ALWAYS runs them - dims that are not multiples of 8
  /// are zero-padded to ceil8(nout)/ceil8(L) by the packing code (see ocudu_mmse_*_matrix.metal
  /// for the padding contract). nn=0 can therefore only mean the matrix engine itself is
  /// unavailable (stale metallib); the CPU path remains the fallback for that case.
  bool use_matrix_engine = false;
  bool matrix_ready      = false;

  /// Whether the last fd/td estimation stage engaged the simdgroup 8x8 kernels
  /// (diagnostics/A-B observability; see nn_engaged_last()).
  bool last_stage_nn = false;

  /// Whether the last fd/td estimation stage merged the tail block into the standard batch
  /// (diagnostics/A-B observability; see merged_batch_last()).
  bool last_stage_merged = false;

  /// K3 (S-6a): the equalizer's channel estimates built on the GPU - per-symbol RE masks, their
  /// prefix RE counts, and the destination [MAX_LAYERS][total_re] cbf16 buffer. The path is
  /// enabled by OCUDU_CE_DEVICE_CE (A/B until the demodulator consumes it) and only engages on
  /// hops whose engine call covers the whole allocation with the legacy kernels.
  static constexpr unsigned MAX_MASK_WORDS = (MAX_NOF_PRBS * NOF_SUBCARRIERS_PER_RB + 31) / 32;
  bool                      device_ce_enabled = false;
  uint32_t*                 gpu_masks         = nullptr; // [MAX_NSYMB_PER_SLOT][MAX_MASK_WORDS]
  uint16_t*                 gpu_ce            = nullptr; // [MAX_LAYERS][MAX_NOF_PRBS * 12 * 14]
  std::array<unsigned, MAX_NSYMB_PER_SLOT + 1> re_offsets{};
  unsigned                  gpu_ce_mask_words = 0;
  unsigned                  gpu_ce_layers     = 0;
  unsigned                  gpu_ce_total_re   = 0;
  bool                      gpu_ce_ready      = false;
  /// Signature of the allocation the staged masks belong to (see stage_re_masks()).
  unsigned mask_first_prb     = ~0u;
  unsigned mask_nof_prb       = 0;
  unsigned mask_hop           = ~0u;
  unsigned mask_rb_pattern    = 0;
  unsigned mask_dmrs_re_bits  = ~0u;
  unsigned mask_dmrs_sym_bits = ~0u;

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
