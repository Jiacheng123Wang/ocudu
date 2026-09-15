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

  /// \brief Device noise variance of the last hop (K4), or nullptr when it was not produced.
  ///
  /// The equalizer scales its soft bits with this value. It is reduced on the device out of the
  /// same h the estimates come from, so a consumer never has to read the grid before dispatching
  /// the equalizer. \note The reduction order is not the host's, so the value matches
  /// get_noise_variance() to floating-point reassociation, not bit for bit.
  const float* device_noise_variance() const { return gpu_nv_ready ? gpu_nv : nullptr; }

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
  bool stage_produces_ls_pilots(const fd_td_estimation_stage_args& args) const override;

  /// \brief The geometry of the hop, as K0-a sees it (see apply_fd_td_estimation_stage()).
  ///
  /// Factored out because TWO decisions read it now - whether the device builds this hop's
  /// least-squares pilots, and whether the host pre-stage runs at all
  /// (stage_produces_ls_pilots()) - and those two answers have to agree by construction.
  struct ls_geometry {
    /// True when the device qualifies for this hop.
    bool     ok         = false;
    unsigned nof_pilots = 0;
    unsigned ncomb      = 0;
    unsigned nof_prb    = 0;
  };
  ls_geometry ls_geometry_of(const fd_td_estimation_stage_args& args) const;

  /// \brief One least-squares pilot of the hop: the device's own result when the device built them
  /// (K0-a), the host pre-stage's buffer otherwise.
  ///
  /// This is the single accessor of the hop's LS pilots for every host consumer of the stage. It
  /// replaced two whole-buffer passes - the copy-back of the device result into pilots_lse_view and
  /// the in-place 1/beta scaling that followed it - which together published a device quantity into
  /// host memory for a handful of readers that need a fraction of it, and left the host buffer as a
  /// second source of truth that the device path had to keep rewriting.
  ///
  /// The DEVICE buffer holds the RECEIVED domain (no DM-RS to data scaling), which is what the
  /// classical FD stage starts from and what the correlation model's noise-to-pilot-power reference
  /// needs; the estimator's published channel is the DATA domain (the classical stage scales by
  /// 1 / beta, and so does the device's own y scatter, see pilots_stage::inv_beta), so a consumer
  /// asks for the domain it needs instead of relying on the buffer having been scaled.
  ///
  /// \param[in] args     Hop arguments (geometry and the DM-RS to data scaling).
  /// \param[in] i_symbol DM-RS symbol of the hop.
  /// \param[in] i_layer  Transmission layer.
  /// \param[in] j        Pilot index within the symbol.
  /// \param[in] scaled   True for the DATA domain, false for the received domain.
  cf_t ls_pilot(const fd_td_estimation_stage_args& args,
                unsigned                        i_symbol,
                unsigned                        i_layer,
                unsigned                        j,
                bool                            scaled) const;

  // See the base class documentation.
  bool complete_fd_td_estimation_stage() override;

  // See the base class documentation.
  bool device_results_cover_last_estimate() const override
  {
    // K3 and K4 write the LAST hop: with frequency hopping the estimates of the earlier hop are not
    // in the device buffers any more, so a consumer must gather those from host memory (and
    // therefore complete the estimation first).
    return gpu_ce_ready && gpu_nv_ready && !last_estimate_hopping;
  }

  // See the base class documentation.
  std::optional<ch_est_device_view> get_device_ch_estimates(unsigned i_symbol, unsigned tx_layer) const override;

  // See the base class documentation.
  const float* get_device_noise_variance() const override { return device_noise_variance(); }

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

  /// \brief Builds A and R_hp of one block geometry on the DEVICE (K0-d).
  ///
  /// Both matrices are analytic (a time correlation times a frequency correlation), so their values
  /// come from the geometry and the three statistics alone - no received sample, no least-squares
  /// estimate, no CFO. The host therefore does not build them: it hands the geometry to the engine,
  /// which fills the very slots stage_engine_group() used to copy them into. \c nout and \c L are
  /// still returned, because the caller needs the block geometry either way.
  ///
  /// \return False when the engine cannot build them (unsupported metallib or geometry), in which
  ///         case the caller falls back to build_correlation_matrices().
  /// \brief The K0-d descriptor of one block geometry: what the engine call needs to build A and
  /// R_hp into the slots itself (no dispatch of its own - see run_weights_only()'s corr stage).
  /// \param[in] a_stride Slot row stride of A (>= L) and, together with \p r_stride, the spacing
  ///                     between the systems of the batch. They are equal to L / nout when the batch
  ///                     owns slots sized for its own geometry; a batch tucked into another
  ///                     geometry's slots (the merged edge block) passes the SLOT's strides while
  ///                     \p L and \p nout describe its own block.
  metal::mmse_engine::corr_stage correlation_stage(const channel_statistics&                     stats,
                                                   const bounded_bitset<NOF_SUBCARRIERS_PER_RB>& re_pattern,
                                                   unsigned                                       b_prb,
                                                   span<const unsigned>                           dmrs_slot_symbols,
                                                   unsigned                                       scs_khz,
                                                   unsigned                                       sys_offset,
                                                   unsigned&                                      nout,
                                                   unsigned&                                      L,
                                                   unsigned                                       a_stride,
                                                   unsigned                                       r_stride);

  bool build_correlation_matrices_device(const channel_statistics&                     stats,
                                         const bounded_bitset<NOF_SUBCARRIERS_PER_RB>& re_pattern,
                                         unsigned                                       b_prb,
                                         unsigned                                       gb_start,
                                         span<const unsigned>                           dmrs_slot_symbols,
                                         unsigned                                       scs_khz,
                                         unsigned                                       sys_offset,
                                         unsigned                                       n_layers,
                                         unsigned&                                      nout,
                                         unsigned&                                      L);

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
  /// \param slots_filled The DEVICE already wrote A and R_hp of these slots (K0-d). It is the ONLY
  ///               gate on the A/R_hp stores, and it covers both matrices: they are written TOGETHER
  ///               or not at all. A separate "skip A but still write R_hp" flag was the defect that
  ///               left R_hp stale on the device-inversion path (2916 of 27216 entries valid, SINR
  ///               -23 dB). The pilot staging below runs either way - except that the DEVICE may be
  ///               the one writing y (glue #2, see record_device_y_stage()), in which case the host
  ///               skips its copy. Deliberately has NO default, so
  ///               every call site has to state which of the two it means.
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
                          bool                               gpu_invert,
                          bool                               slots_filled);

  /// \brief Glue #2 (S-7f-5u): records that the DEVICE writes this group's y slots, so that
  /// stage_engine_group() must NOT copy the pilots in from the host.
  ///
  /// \return True when the device will write them, i.e. when the host staging must be skipped. It
  ///         is the ONLY such gate, and it is answered here - where the block geometry (gb_start,
  ///         b_prb, npf), the batch strides and the system offset all live - rather than by the
  ///         callers, so that "the device wrote y" cannot be derived differently in two places.
  ///
  /// The descriptor it appends is picked up by engine_run() and encoded into the engine's own
  /// command buffer (the scatter is a dispatch of THAT buffer, not a submission of its own).
  /// A merged batch stages two groups and therefore records two descriptors.
  ///
  /// It answers false - and the host stages y exactly as before - whenever the device cannot be
  /// the writer: the device LSE is not valid for this hop (K0-a did not run or failed), the
  /// path is switched off (OCUDU_CE_DEV_Y=0, the A/B), the metallib has no scatter kernel, or the
  /// geometry does not fit the buffers.
  bool record_device_y_stage(const fd_td_estimation_stage_args& args,
                             unsigned                           gb_start,
                             unsigned                           n_blk,
                             unsigned                           b_prb,
                             unsigned                           npt,
                             unsigned                           L,
                             unsigned                           sys_offset,
                             const engine_strides&              st);

  /// \brief One engine call (one command buffer, one commit/wait) over the staged slots, with the
  /// optional K3 reformat stage appended to the same command buffer.
  /// \param reformat Per-symbol mask/offsets and destination of the equalizer's estimates, or
  ///                 nullptr to skip K3. Only meaningful for the legacy (non-matrix) kernels.
  /// \return False when the engine call failed (wrap/commit error), in which case the caller MUST
  ///         fall back to the CPU reference math for these blocks (S-1 audit fix: the return value
  ///         was previously ignored, which could silently leave stale channel estimates in the
  ///         grid).
  /// \param defer Submit without waiting when the kernels allow it (the legacy path does; the
  ///              matrix flavor and the CPU-inversion A/B knob do not, and complete the batch
  ///              inline). A deferred batch must be completed by complete_fd_td_estimation_stage()
  ///              before anything reads its results - including their unpack into the grid - and
  ///              before another batch of the same hop is submitted, because they share the gpu_h
  ///              staging buffer this call overwrites. That is what the call below enforces: only
  ///              the LAST batch of a hop stays outstanding.
  /// \param[in] defer Submit the batch without waiting for it (see port_channel_estimator::submit()):
  ///                  the caller collects it through complete_fd_td_estimation_stage(). Deliberately
  ///                  has NO default: a defaulted false here silently turned the merged
  ///                  standard+tail path - the one every wide hop takes, and the only path the air
  ///                  interface exercises - into a synchronous wait (~280us per hop), because that
  ///                  call site omitted the argument while the unpack beside it used the flag.
  /// \param corr K0-d descriptor to encode as a PREFIX of the engine's own command buffer, or
  ///             nullptr. Only valid when the slots are left holding A (the device-inversion
  ///             route); see mmse_engine::run_async() for why.
  bool engine_run(const metal::mmse_engine::corr_stage*      corr,
                  unsigned                                 nout,
                  unsigned                                 L,
                  unsigned                                 nof_systems,
                  unsigned                                 nof_blocks,
                  bool                                     matrix,
                  bool                                     gpu_invert,
                  const metal::mmse_engine::reformat_stage* reformat,
                  bool                                     defer,
                  const engine_strides&                    st,
                  unsigned                                 sys_offset);

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
  /// \param[in] device_stats When non-null, the DEVICE builds A and R_hp of this batch (K0-d) and
  ///                        the host then inverts them in the slots - the full-GPU-path form of the
  ///                        correlation matrices. Null keeps the host staging.
  /// Whether the inversion runs on the DEVICE for a block of this order.
  ///
  /// **DEFAULT ON**: the full-GPU-path form, and the reason the goal exists.
  /// K1's accuracy is not an obstacle: the device inverse's W error is 7.67e-4 against the host's
  /// 1.40e-5, worth about 0.003 dB of SINR, while the -18.7 dB that once looked like an accuracy
  /// failure was two staging defects (S-7f-3i, S-7f-4x). What kept the default off was that only
  /// _some_ hops came out right; the actual defect was in the STAGING, not in K1 - a per-flag skip
  /// that stopped the host from writing R_hp whenever the device was to invert, so the weights ran
  /// on the previous hop's residue (2916 of 27216 entries, h 1440 of 8064, SINR -23 dB). With
  /// stage_engine_group() gated on `slots_filled` alone, the device path measures the same as the
  /// host's on every capture, so there is no functional reason left to keep it off.
  ///
  /// Cost (a PERFORMANCE debt, tracked separately, not a blocking defect). It is NOT the barriers:
  /// S-5a measured that removing 36 of the kernel's 72 barriers saves 2.5us, while the THREADGROUP
  /// GEOMETRY moves the very same kernel over a 3.7x range - (32,4) 91.3us against (64,16) 24.5us on
  /// one 36x36 system. The default path used to dispatch the worst of the six. mmse_inv_threadgroup()
  /// (ocudu_metal_mmse_engine.mm) now picks the measured best for every K1 dispatch, which took the
  /// whole-engine wait from 357/212/284us to 121/107/195us on the three reference captures.
  ///
  /// \param[in] order Block order L. Above the kernel's own MAX_N (mmse_inv.metal: 54) the kernel
  ///                  cannot be dispatched at all, so the host inversion is the only option there.
  /// ESCAPE HATCHES (both must keep working): OCUDU_CE_GPU_INVERT=0 forces the host inversion, and
  /// OCUDU_CE_CPU_INVERT=1 does the same from the other side (it also disables the device build's
  /// inversion step, so the pair is unambiguous).
  static bool device_inverts(unsigned order)
  {
    static const bool enabled = []() {
      const char* env = std::getenv("OCUDU_CE_GPU_INVERT");
      return (env == nullptr) || (std::strtoul(env, nullptr, 10) != 0);
    }();
    static constexpr unsigned MAX_DEVICE_INVERT_ORDER = 54;
    return enabled && (std::getenv("OCUDU_CE_CPU_INVERT") == nullptr) && (order <= MAX_DEVICE_INVERT_ORDER);
  }

  /// \brief K0-d: the device builds A and R_hp of one block geometry into the engine slots.
  ///
  /// The build completes HERE, in its own command buffer, and leaves the L x L / nout x L blocks of
  /// A and R_hp in the slots, at the slot row stride \c a_stride. Removes the host's construction of
  /// both matrices (two nested correlation loops over 170 KB of stores).
  ///
  /// When \c gpu_invert is set, the slots keep A and K1 inverts them inside the weights command
  /// buffer (one round trip less); otherwise the host writes A^-1 over A in place here, which is
  /// what run_weights_only() expects to read.
  ///
  /// \param[in] gpu_invert The SAME decision run_engine_blocks() gives the staging and the engine
  ///             call (device_inverts()). Passed in rather than re-derived: deriving it here from
  ///             the order alone let the matrix flavor - which never routes to K1 - leave a raw A
  ///             where the weights expected an inverse.
  /// \return True when the slots are filled and ready for the weights. False means the caller must
  ///         fall back to its own staging - the return value is the success flag, never a descriptor
  ///         (reading it as one was a defect: the success path used to leave it nullopt).
  bool build_slots_on_device(const channel_statistics& stats,
                             const bounded_bitset<NOF_SUBCARRIERS_PER_RB>& re_pattern,
                             unsigned                                       b_prb,
                             span<const unsigned>                           dmrs_slots,
                             unsigned                                       scs_khz,
                             unsigned                                       sys_offset,
                             unsigned                                       nof_systems,
                             unsigned                                       a_stride,
                             unsigned                                       r_stride,
                             unsigned                                       L,
                             bool                                           gpu_invert);

  /// \param[in] sys_offset  First engine slot of this batch. The standard blocks start at 0; the
  ///                        edge/tail block of a hop sits at nof_layers, and the device build has to
  ///                        write ITS slots or it would clobber the standard group's.
  /// \param[in] device_stats When non-null, the DEVICE builds A and R_hp of this batch (K0-d) and
  ///                        the host then inverts them in the slots - the full-GPU-path form of the
  ///                        correlation matrices. Null keeps the host staging.
  bool run_engine_blocks(const fd_td_estimation_stage_args& args,
                         unsigned                           gb_start,
                         unsigned                           n_blk,
                         unsigned                           b_prb,
                         unsigned                           nout,
                         unsigned                           L,
                         unsigned                           npt,
                         bool                               matrix,
                         const metal::mmse_engine::reformat_stage* reformat   = nullptr,
                         bool                               defer        = false,
                         const channel_statistics*          device_stats = nullptr,
                         unsigned                           sys_offset   = 0);

  /// \brief Unpack of a batch whose command buffer has not been waited for yet.
  ///
  /// The estimates are in gpu_h once that command buffer completes, so a deferred batch records
  /// where they go and complete_fd_td_estimation_stage() unpacks them - waiting first, because
  /// gpu_h is a shared staging buffer: the next hop overwrites it.
  struct pending_unpack {
    unsigned       gb_start   = 0;
    unsigned       n_blk      = 0;
    unsigned       b_prb      = 0;
    unsigned       nout       = 0;
    unsigned       nof_layers = 0;
    unsigned       sys_offset = 0;
    engine_strides st{};
  };

  /// A hop has at most two batches (the standard blocks and the tail block, or the merged pair).
  static constexpr unsigned max_pending_unpacks = 2;

  /// Records the unpack of a batch submitted without waiting (see engine_run()).
  void defer_unpack(unsigned              gb_start,
                    unsigned              n_blk,
                    unsigned              b_prb,
                    unsigned              nout,
                    unsigned              nof_layers,
                    unsigned              sys_offset,
                    const engine_strides& st);

  std::array<pending_unpack, max_pending_unpacks> pending_unpacks{};
  unsigned                                        nof_pending_unpacks = 0;
  /// True while a batch submitted by the last stage call is still outstanding.
  bool stage_pending = false;

  /// \brief The pilot-derived buffers of a hop, filled out of the estimated grid.
  ///
  /// RSrp, the noise variance and the time alignment are computed by the base class from these, and
  /// they are read from the grid the batch writes - so they can only be filled once that batch has
  /// completed, which is why a deferred stage records what to fill here (see
  /// complete_fd_td_estimation_stage()). The destinations are views into buffers the caller keeps
  /// alive until the hop is completed.
  struct pending_fill {
    bool     valid      = false;
    unsigned nof_prb    = 0;
    unsigned npt        = 0;
    unsigned nof_layers = 0;
    std::array<unsigned, MAX_NOF_DMRS_SYMBOLS>                    dmrs_sym{};
    std::array<bounded_bitset<NOF_SUBCARRIERS_PER_RB>, MAX_LAYERS> re_pattern{};
    std::array<span<cf_t>, MAX_NOF_DMRS_SYMBOLS * MAX_LAYERS>      filtered_dst{};
    std::array<span<cf_t>, MAX_NOF_DMRS_SYMBOLS * MAX_LAYERS>      freq_dst{};

    /// Copies the pilot REs and the DM-RS symbol slices of every layer out of the estimated grid.
    void fill(const static_re_buffer<MAX_LAYERS * MAX_NSYMB_PER_SLOT, MAX_NOF_SUBCARRIERS>& grid) const;
  };

  pending_fill deferred_fill;

#if defined(OCUDU_CE_TIME)
  /// Measurements of a stage whose batch is still outstanding ([mmse_time_sum], debug aid).
  ///
  /// The GPU busy time of a deferred batch and the wall time the caller spends waiting for it are
  /// only known when complete_fd_td_estimation_stage() runs, so the stage leaves what it measured
  /// here and the completion finishes the accounting (see mmse_stats_accumulate()).
  struct deferred_stage_stats {
    bool     valid = false;
    unsigned nof_prb = 0;
    unsigned npt = 0;
    bool     hop_gpu = false;
    bool     hop_nn = false;
    unsigned fallback_blocks = 0;
    /// CPU time of the hop before the stage (pilot extraction + LSE + CFO): what a device-side pilot
    /// extraction takes over.
    double   pre_stage_us = 0.0;
    /// Time spent copying the coefficient matrices and pilot vectors into the engine slots.
    double   stage_us = 0.0;
    /// Time spent encoding and submitting the engine batch, and unpacking its K3 results.
    double   submit_us = 0.0;
    double   unpack_us = 0.0;
    double   sigma2_us = 0.0;
    double   corr_us = 0.0;
    /// Stage start -> end of the stage's CPU work, i.e. the GPU phase without the deferred wait.
    double gpu_path_us = 0.0;
    double cpu_blocks_us = 0.0;
    /// The same window: the hop without the deferred wait.
    double total_us = 0.0;
  };
  deferred_stage_stats                  deferred_stats;
  std::chrono::steady_clock::time_point deferred_wait_begin{};
#endif

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

  /// K3 (S-6a): the equalizer's channel estimates built on the GPU - the destination
  /// [MAX_LAYERS][total_re] cbf16 buffer and the layout (per-symbol RE counts and the DM-RS comb)
  /// K3 derives the destination indices from. The path is on by default (the demodulator consumes
  /// the estimates) and only engages on hops whose engine call covers the whole allocation with
  /// the legacy kernels; OCUDU_CE_CPU_CE=1 keeps both sides on the host gather for A/B.
  bool                      device_ce_enabled = false;
  uint16_t*                 gpu_ce            = nullptr; // [MAX_LAYERS][MAX_NOF_PRBS * 12 * 14]
  std::array<unsigned, MAX_NSYMB_PER_SLOT + 1> re_offsets{};
  unsigned                  gpu_ce_drpp          = 0;
  unsigned                  gpu_ce_drpp_dmrs     = 0;
  unsigned                  gpu_ce_dmrs_re_bits  = 0;
  unsigned                  gpu_ce_dmrs_sym_bits = 0;
  unsigned                  gpu_ce_layers     = 0;
  unsigned                  gpu_ce_total_re   = 0;
  bool                      gpu_ce_ready      = false;
  /// True when the last estimation stage covered the second hop of a frequency-hopping allocation.
  bool                      last_estimate_hopping = false;

  /// K4 (S-6c-0): the equalizer's noise variance reduced on the device, and the pilot inputs the
  /// reduction reads (staged by the estimator, [npt][layers or groups][npf] complex each).
  float* gpu_nv        = nullptr;
  float* gpu_pilots    = nullptr;
  float* gpu_rx_pilots = nullptr;
  float* gpu_epochs    = nullptr;
  /// K0-a staging: the transmitted DM-RS of the hop and the device's least-squares pilots, both
  /// [symbol][layer][pilot] real/imag interleaved, plus the CFO scalar it estimates. Page-aligned so
  /// the kernels can be handed them without a copy.
  /// Elements of one K0-a staging buffer: [symbol][layer][pilot] real/imag interleaved.
  static constexpr std::size_t k_ls_floats =
      2 * static_cast<std::size_t>(MAX_DMRS_SYMBOLS) * MAX_LAYERS * MAX_NOF_PILOTS_SYMBOL;
  float* gpu_ls_ref    = nullptr;
  float* gpu_ls_out    = nullptr;
  float* gpu_ls_cfo    = nullptr;
  bool   gpu_nv_ready  = false;

  /// S-7f-5w: the hop's noise variance, computed inside the K0-a command buffer. \c gpu_ls_smoothed
  /// is the frequency-smoothed copy of the pilots the classical estimator reads (a scratch buffer:
  /// the LSE itself must survive for the weights' y vectors), \c gpu_ls_sigma2 is the single float
  /// the kernels leave behind, and \c fd_filter holds the raised-cosine coefficients of the hop's
  /// geometry (a host-side table, see get_fd_smoothing_filter()).
  float*                    gpu_ls_smoothed = nullptr;
  float*                    gpu_ls_sigma2   = nullptr;
  std::array<float, 32>     fd_filter{};
  unsigned                  fd_filter_len = 0;
  /// Whether the host asks the device for sigma2 (OCUDU_CE_DEV_SIGMA2=0 keeps the host computation,
  /// which is also the A/B of capture_gates.sh sig2), and whether THIS hop got it.
  bool device_sigma2_enabled = false;
  bool device_sigma2_valid   = false;

  /// \brief Stages the received DM-RS of the hop and the symbol epochs - the arrays the device noise
  /// variance reads, and the same ones K4 reads.
  /// \return The number of CDM groups staged, or 0 when the geometry does not fit the buffers.
  unsigned stage_device_noise_inputs(const fd_td_estimation_stage_args& args, unsigned npt);

  /// Glue #2 (S-7f-5u): whether the device writes the engine's pilot vectors y out of gpu_ls_out
  /// instead of the host copying them in. On by default; OCUDU_CE_DEV_Y=0 keeps the host staging,
  /// which is the A/B of the two writers (capture_gates.sh ydev: the two must publish
  /// byte-identical output, because the kernel only re-indexes and applies the same inv_beta
  /// product the host applies).
  bool device_y_enabled = false;
  /// Whether gpu_ls_out holds THIS hop's pilots (K0-a ran and succeeded). It is set once per hop by
  /// the K0-a stage and is what makes the device y write legal: the scatter reads exactly this
  /// buffer, and a hop whose device LSE failed has nothing there.
  bool device_ls_valid = false;
  /// The descriptors of the groups the device will write, collected by stage_engine_group() and
  /// consumed by engine_run(). At most two: a merged batch stages the standard group and the edge
  /// group before its single engine call.
  static constexpr unsigned k_max_y_scatter = 2;
  std::array<metal::mmse_engine::pilots_scatter, k_max_y_scatter> device_y_stage{};
  unsigned                                                        nof_device_y_stage = 0;
  /// A copy of the descriptors of the LAST batch, kept for the OCUDU_CE_Y_CHECK probe below (the
  /// staging list itself is consumed by engine_run()).
  std::array<metal::mmse_engine::pilots_scatter, k_max_y_scatter> device_y_stage_last{};
  unsigned                                                        nof_device_y_stage_last = 0;

  /// \brief OCUDU_CE_Y_CHECK=1 probe: compares the y the DEVICE wrote against the values the host
  /// staging would have written, element by element (the same comparison capture_gates.sh ydev
  /// makes end to end, but per slot and per index).
  ///
  /// The device write happens inside a command buffer that may still be in flight, so the probe
  /// completes the stage first. It is a diagnostic for the one thing a byte-identical end-to-end
  /// gate cannot localize: WHICH slot differs.
  /// \return True when every element matches.
  bool probe_device_y_stage(const fd_td_estimation_stage_args& args);

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
