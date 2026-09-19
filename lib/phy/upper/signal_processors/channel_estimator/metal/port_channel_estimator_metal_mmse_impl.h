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
#include "ocudu_mmse_refusals.h"
#include "ocudu/ocudulog/ocudulog.h"

namespace ocudu {

/// 2D block MMSE port channel estimator (Apple Silicon Metal skeleton, CPU reference math).
class port_channel_estimator_metal_mmse_impl : public port_channel_estimator_average_impl
{
public:
  /// \brief How many hops the OCUDU_CE_TA_CHECK probe has judged WRONG (batch 5b's A/B verdict).
  ///
  /// A process-wide counter rather than a return value: the probe runs inside the completion, which the
  /// estimator's own callers drive, and this is what an offline harness (the unit test) reads to make
  /// the A/B a PASS/FAIL instead of a printout. Zero when the probe never ran - which is not a pass,
  /// and is why the caller checks that it ran as well (see device_ta_probe_checks()).
  static unsigned device_ta_probe_failures();

  /// How many hops the OCUDU_CE_TA_CHECK probe has compared.
  static unsigned device_ta_probe_checks();

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

  /// \brief Whether this hop's received pilots are built on the device (S13-P2).
  ///
  /// It is the SAME gate as stage_produces_ls_pilots() above - and the same one
  /// apply_fd_td_estimation_stage() uses to decide whether to stage the received pilots or leave them
  /// to the extraction kernel - so the three can never disagree: the kernel that builds the
  /// least-squares pilots reads the received ones on its way and stores them (mmse_pilots_lse).
  /// \note The one extra condition is the EPRE reduction's kernel: without it the device would build
  ///       the pilots but publish no EPRE sum, and the base class would have nothing to accumulate on a
  ///       hop it did not extract. A metallib without it therefore keeps the host extraction.
  bool stage_produces_hop_inputs(const fd_td_estimation_stage_args& args) const override;

  // See the base class documentation.
  std::optional<float> get_device_epre_sum() const override;

  /// \brief Reports the hop's received pilots being read out of the grid HERE, where the grid is the
  /// DEVICE's (the front-end DFT writes it - S-7b), so the extraction the base class performs on
  /// every hop is a device -> host read even though no byte crosses a bus (it is unified memory).
  ///
  /// It is a crossing by the contract's own definition and the counter could not see it before: the
  /// extraction is unconditional, and no site was instrumented on it. Together with the write site in
  /// stage_device_noise_inputs() (which hands the same values back to the device) this makes the
  /// round trip visible - and it is the reason the "zero crossings" claim is a claim about the
  /// AUDITED paths and not about the lane.
  void account_host_grid_read(unsigned nof_re, bool device_written) override;

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

  /// Hands the hop's device-side rsrp sum to compute_hop_finish() when the device produced one for
  /// this hop (see the base class for why this exists: it is what keeps the read-back grid out of
  /// the reporting path). Nullopt when the device statistics are off or the hop has no reduction.
  std::optional<float> get_device_rsrp_sum(unsigned i_layer) const override;

  // See the base class documentation.
  bool device_results_cover_last_estimate() const override
  {
    // K3 and K4 write the LAST hop: with frequency hopping the estimates of the earlier hop are not
    // in the device buffers any more, so a consumer must gather those from host memory (and
    // therefore complete the estimation first).
    return gpu_ce_ready && gpu_nv_ready && !last_estimate_hopping;
  }

  // See the base class documentation. The reason is the FIRST refusal of this hop (see
  // mmse_refusals::begin_hop), and "no reason at all" is itself a finding: the device did not cover
  // the hop and nothing said why (frequency hopping is that case today - the earlier hop's buffers
  // are gone - and a consumer must NOT read it as "fine").
  const char* device_shortfall_reason() const override
  {
    if (device_results_cover_last_estimate()) {
      return nullptr;
    }
    std::optional<metal::mmse_refusal> reason = metal::mmse_refusals::first_hop_reason();
    return reason.has_value() ? metal::to_string(*reason)
                              : "unattributed (the device did not cover this hop and no stage said why)";
  }

  // See the base class documentation: a refusal by a KNOB is an arm the operator asked for, not a hop
  // the device could not serve.
  bool device_shortfall_is_knob_requested() const override
  {
    if (device_results_cover_last_estimate()) {
      return false;
    }
    std::optional<metal::mmse_refusal> reason = metal::mmse_refusals::first_hop_reason();
    return reason.has_value() && metal::is_knob_refusal(*reason);
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
  /// \param pad_y_slots When true, this group carries fewer real block slots than its slot
  ///               geometry (the merged tail: n_blk real slots inside st.n_blk), so the pad slots
  ///               must hold zeros rather than a previous hop's pilots. Cleared HERE, and only when
  ///               the HOST is the one staging y: when record_device_y_stage() takes the group, the
  ///               scatter kernel zeroes those very slots inside the command buffer whose weights
  ///               read them, so clearing them on the host would be a device crossing spent on memory
  ///               the device is about to write.
  ///
  ///               \note It is a FLAG, not an extent, and that is deliberate. The region is derived
  ///               inside from st.n_blk / st.L - the same strides the staging loop and the apply
  ///               kernel use. An extent computed by the caller would be written in the caller's
  ///               convention (n_std_blocks / L_std), and the two are equal only while
  ///               st.n_blk == n_std_blocks and st.L == L_std: a base pointer in one unit with a
  ///               length in the other is a silent corruption waiting for the day they diverge.
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
                          bool                               slots_filled,
                          bool                               pad_y_slots = false);

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
                  unsigned                                 sys_offset,
                  const metal::mmse_engine::corr_stage*    corr_edge = nullptr);

  /// \brief Unpacks the engine outputs of the group staged at \c sys_offset into the grid
  /// (symbol-major within each block; the blocks start at PRB gb_start).
  ///
  /// \param[in] all_symbols True unpacks the whole slot grid, false only the hop's DM-RS symbols -
  ///            which is what the hop statistics read, and the only part the air path needs (see
  ///            materialize_host_grid()).
  void unpack_engine_group(unsigned              gb_start,
                           unsigned              n_blk,
                           unsigned              b_prb,
                           unsigned              nout,
                           unsigned              nof_layers,
                           unsigned              sys_offset,
                           const engine_strides& st,
                           bool                  all_symbols) const;

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
  /// \brief Compares the edge group's device-built slots against the host's own build of the same
  /// geometry, in the SAME process and on the SAME hop (OCUDU_CE_EDGE_CHECK).
  ///
  /// The S4 defect was localised by differencing published dumps across runs: 6043 bytes, confined to
  /// the edge region of _h, and every candidate explanation had to be argued away from the outside.
  /// That is the wrong instrument - the two builds are available at the same instant, from the same
  /// inputs, in one process, so they can simply be compared element by element. This is that
  /// comparison, and it answers the only question that matters first: is the device's edge SLOT the
  /// host's edge slot, and if not, from which element do they part.
  ///
  /// The host build goes into w_r_pp / w_r_hp (which the device route leaves unused) and the device's
  /// slots are read back through the same mapping the kernels wrote, so the comparison covers the
  /// whole slot - including the pads, which the host writes in one form and stage_engine_group() in
  /// the other. That makes a pad disagreement visible instead of silent.
  ///
  /// \param[in] nout_e     R_hp rows of the edge block, as correlation_stage() reported them.
  /// \param[in] L_e        Matrix order of the edge block.
  /// \param[in] sys_offset First system of the edge group (nof_layers).
  /// \param[in] nof_systems Systems in the edge group (nof_layers).
  /// \param[in] a_stride   Slot strides the group lives in (engine_strides::L / ::nout). They belong
  ///            to the STANDARD geometry and exceed the edge block's own L_e / nout_e - which is the
  ///            whole point of the check, so they are passed in rather than derived.
  /// \return True when every element of every edge slot is bit-identical.
  /// \brief One device-built correlation group waiting for its slot comparison (OCUDU_CE_CORR_CHECK /
  /// OCUDU_CE_EDGE_CHECK).
  ///
  /// WHY DEFERRED. A group whose build rides ANOTHER command buffer - the fused prefix of a merged
  /// batch - has NOT been dispatched when the caller hands it over, so comparing its slots at that
  /// moment reads memory the GPU has not written yet. That is why the older checks were disabled for
  /// that route (and the edge one ran only where a standalone build had already completed), which left
  /// the fused path with no instrument at all: measured on syn004_4, the fused edge block comes out
  /// NaN and nothing in the build said so (design document, P0 of the control-plane fusion).
  ///
  /// The record therefore carries the inputs, and the comparison runs where every writer has finished:
  /// complete_fd_td_estimation_stage(), before the unpack reads the results.
  struct pending_corr_check {
    /// Which group this is, for the report ("standard" / "edge").
    const char* which = "";
    /// Inputs of the host's own build of the same geometry.
    channel_statistics                            stats{};
    bounded_bitset<NOF_SUBCARRIERS_PER_RB>        re_pattern{};
    static_vector<unsigned, MAX_NOF_DMRS_SYMBOLS> dmrs_slots;
    unsigned                                      b_prb       = 0;
    unsigned                                      scs_khz     = 0;
    /// What the group occupies.
    unsigned nout        = 0;
    unsigned l           = 0;
    unsigned sys_offset  = 0;
    unsigned nof_systems = 0;
    unsigned a_stride    = 0;
    unsigned r_stride    = 0;
    /// Slot count of the batch, for the h window (engine_strides::n_blk).
    unsigned nof_blocks = 0;
  };

  /// Compares every pending group's device slots against the host's own build of the same geometry,
  /// and clears the list. Called where the writers have completed (see pending_corr_check).
  void run_pending_corr_checks();

  bool check_edge_slots(const channel_statistics& stats,
                        const bounded_bitset<NOF_SUBCARRIERS_PER_RB>& re_pattern,
                        unsigned                                       b_prb,
                        span<const unsigned>                           dmrs_slots,
                        unsigned                                       scs_khz,
                        unsigned                                       nout_e,
                        unsigned                                       L_e,
                        unsigned                                       sys_offset,
                        unsigned                                       nof_systems,
                        unsigned                                       a_stride,
                        unsigned                                       r_stride);

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
                             bool                                           gpu_invert,
                             metal::mmse_engine::corr_stage*                fused_corr = nullptr);

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
    /// \brief Ring slot of gpu_rsrp this batch's device reduction wrote, or "none".
    ///
    /// Kept per BATCH and not in one member of the estimator, because rsrp_base_ names the hop being
    /// STAGED while the completion of an earlier hop runs after later hops have been staged: reading
    /// the reduction through the staging counter reads whatever block was claimed last. That is the
    /// defect that made a correct device reduction read back as 0 (batch 5a), and per-batch is the
    /// only place the right block is unambiguous.
    unsigned       rsrp_block = kNoRsrpBlock;
    /// Block slots that region holds (nullptr-style zero when there is no reduction).
    unsigned       rsrp_slots = 0;
  };

  /// Value of pending_unpack::rsrp_block when the device produced no reduction for that batch.
  static constexpr unsigned kNoRsrpBlock = ~0u;

  /// A hop has at most two batches (the standard blocks and the tail block, or the merged pair).
  static constexpr unsigned max_pending_unpacks = 2;

  /// \brief Reserves the gpu_rsrp region for one batch and returns the stage that writes it.
  ///
  /// One BLOCK SLOT (kRsrpSlots floats) per standard block plus the edge block's own run, because
  /// the kernel addresses the ring by block slot - and a merged batch gives the standard and the
  /// edge geometry the same blk, so a single slot would have them overwrite each other.
  ///
  /// \param n_blk   Standard blocks per system of this batch (the reformat's n_blk).
  /// \param nf_std  Subcarriers of a standard block.
  /// \param nf_tail Subcarriers of the edge block, 0 when the batch carries none.
  /// \param nof_layers Layers of the batch (a block slot holds two floats per layer).
  /// \return The stage to hand to engine_run(), with its dst and combs filled in, or an empty one
  ///         when the device statistics are off.
  metal::mmse_engine::reformat_stage::rsrp_stage_t rsrp_attach_stage(unsigned n_blk,
                                                                    unsigned nf_std,
                                                                    unsigned nf_tail,
                                                                    unsigned nof_layers);

  /// \brief Builds the optional device time-alignment stage (K7+K6) for the hop being staged.
  ///
  /// Derives the transform size, the pilot stride and the search window with the HOST's own formulas
  /// (time_alignment_estimator_dft_impl::get_idft() and estimate_ta_correlation()), refuses the
  /// geometries the kernel does not cover (the sparse-mask route and an unrecognised comb), and
  /// reserves this hop's rotating slot. An empty stage (dst == nullptr) leaves the host's estimator as
  /// the only source of the value - which then also keeps the grid read-back alive for this hop, since
  /// the host's route needs the pilots (see host_grid_published()).
  metal::mmse_engine::reformat_stage::ta_stage_t ta_attach_stage(const fd_td_estimation_stage_args& args);

  /// \brief The hop's device time alignment, for the base class to publish (see the hook's note).
  std::optional<float> get_device_ta_seconds() const override;


  /// Records the unpack of a batch submitted without waiting (see engine_run()).
  void defer_unpack(unsigned              gb_start,
                    unsigned              n_blk,
                    unsigned              b_prb,
                    unsigned              nout,
                    unsigned              nof_layers,
                    unsigned              sys_offset,
                    const engine_strides& st,
                    unsigned              rsrp_block = kNoRsrpBlock,
                    unsigned              rsrp_slots = 0);

  std::array<pending_unpack, max_pending_unpacks> pending_unpacks{};
  unsigned                                        nof_pending_unpacks = 0;
  /// True while a batch submitted by the last stage call is still outstanding.
  bool stage_pending = false;

  /// \brief Whether the outstanding batch was encoded into the command buffer the receiving chain
  /// shares, rather than into the engine's own (see fused_burst_hop).
  ///
  /// Kept beside \c stage_pending and written where that one is written, because the completion has
  /// to take the matching route: the engine's wait_pending() cannot see a burst, and the burst's
  /// completion cannot see a command buffer of the engine's own.
  bool pending_fused_burst = false;

  /// \brief Whether the hop being staged hands its dispatches to the shared burst (S-7g-16, Step 1b).
  ///
  /// Set at the top of apply_fd_td_estimation_stage() and constant for that hop. True only for a hop
  /// the caller left running (see fd_td_estimation_stage_args::deferred) and only in ce_lane_order::burst;
  /// every other hop keeps a command buffer of the engine's own, which is what the host consumers of the
  /// estimates need.
  bool fused_burst_hop = false;

  /// The order this adapter hands to the engine for a deferred hop: OCUDU_CE_LANE_ORDER, whose default is
  /// ce_lane_order::event (the estimator commits into its own command buffer as soon as it is encoded and
  /// the lane burst waits for it through the back-end stage fence). See ce_lane_order for the three
  /// orders and what each costs, and ce_lane_order_from_env() for the escape hatches.
  static metal::ce_lane_order ce_lane_order_from_env();

  /// \brief Unpack of the last hop, kept so a HOST consumer of the estimates can still be served.
  ///
  /// The device's own estimates are the source of truth while a hop is current: the demodulator
  /// reads them through get_device_ch_estimates() and the hop statistics are derived from the DM-RS
  /// symbols alone. The host copy of the whole 14-symbol grid therefore has exactly three
  /// consumers, all of them fallbacks - get_symbol_ch_estimate() when the device view is missing,
  /// the CPU block path, and the OCUDU_UL_DUMP capture - and S-7f-6a stopped materializing it
  /// eagerly: the DM-RS symbols are unpacked when the hop completes (the statistics need them) and
  /// the rest is unpacked by the first call that asks for it.
  ///
  /// A hop that HOPS is the exception, and it is why the descriptors are kept at all: hop 0's
  /// estimates must be published before hop 1's batch overwrites the device buffers they would be
  /// read from, so a hopping hop is unpacked in full when it completes (host_grid_pending stays
  /// false).
  void materialize_host_grid() const;

  /// Descriptors of the last hop's device batches, and whether their non-DM-RS symbols are still
  /// waiting for a host consumer (see materialize_host_grid()).
  mutable std::array<pending_unpack, max_pending_unpacks> host_unpacks{};
  mutable unsigned                                        nof_host_unpacks   = 0;
  mutable bool                                            host_grid_pending  = false;

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

  /// Per-layer pilot combs of the last staged hop (K5's input), one 12-bit mask per layer.
  std::array<unsigned, 4> gpu_ce_pilot_re_bits{};

  /// Block slots the last batch carried (K5's n_blk) and the last hop's PRB count: what the K5 probe
  /// needs to sum the device's per-block values and to walk the host grid over the same REs.
  unsigned reformat_blocks_last = 0;
  unsigned last_stage_nof_prb   = 0;
  /// Per-layer pilot patterns of the last hop, for the same probe (the combs above are the masks the
  /// kernel takes; these are what the host walks, and keeping both is what lets the probe compare the
  /// same REs rather than trusting that two encodings of a comb agree).
  std::array<bounded_bitset<NOF_SUBCARRIERS_PER_RB>, 4> last_stage_layer_re_pattern{};
  /// The TA stage of the last hop: the stride and the spacing the probe re-runs the HOST's estimator
  /// with, and the hops' DM-RS symbols (the slices it must feed it, in the same order).
  unsigned            ta_probe_stride  = 0;
  subcarrier_spacing  ta_probe_scs     = subcarrier_spacing::kHz15;
  unsigned            ta_probe_symbols = 0;
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
  /// \brief The two scalars the device kernels derive the hop's symbol start epochs from (batch 5g).
  ///
  /// Replaces the 14-float array that used to be uploaded into gpu_epochs. The values are a function
  /// of (numerology, cyclic prefix) alone, so the kernels compute the few they need themselves
  /// (ocudu_mmse_epochs.h) instead of the host writing a device buffer - which was the last host ->
  /// device write in the lane, and the reason the crossing contract could not reach 8 of 8.
  struct epoch_geometry {
    unsigned numerology  = 0;
    bool     cp_extended = false;

    bool operator==(const epoch_geometry& other) const
    {
      return (numerology == other.numerology) && (cp_extended == other.cp_extended);
    }
  };

  /// \brief \p args' epoch geometry, checked against the host's own array ONCE per distinct geometry.
  ///
  /// The device derives the epochs from the same rule this file's restatement of it encodes
  /// (ocudu_mmse_epochs.h), and the host array is the reference that rule has to reproduce: it is the
  /// same array the device used to READ, so a single differing bit here is a change in what every CFO
  /// phasor in K4 and K0-a rotates by. The comparison is fourteen floats of host memory - no device
  /// buffer, no GPU work, no read-back - so it runs unconditionally, prints its verdict, and is not a
  /// crossing. The unit test sweeps the whole (cp, scs) domain against cyclic_prefix::get_length();
  /// this checks the one geometry the running cell actually uses.
  epoch_geometry epoch_geometry_of(const fd_td_estimation_stage_args& args);

  /// The last geometry epoch_geometry_of() judged, and whether it judged one yet.
  epoch_geometry checked_epoch_geometry{};
  bool           checked_epoch_geometry_valid = false;
  /// K0-a staging: the transmitted DM-RS of the hop and the device's least-squares pilots, both
  /// [symbol][layer][pilot] real/imag interleaved, plus the CFO scalar it estimates. Page-aligned so
  /// the kernels can be handed them without a copy.
  /// Elements of one K0-a staging buffer: [symbol][layer][pilot] real/imag interleaved.
  static constexpr std::size_t k_ls_floats =
      2 * static_cast<std::size_t>(MAX_DMRS_SYMBOLS) * MAX_LAYERS * MAX_NOF_PILOTS_SYMBOL;
  float* gpu_ls_ref    = nullptr;
  float* gpu_ls_out    = nullptr;
  /// \brief Rotating slots holding the hop's CFO - one \c kCfoSlots-float buffer, not a single float.
  ///
  /// The extraction writes this hop's CFO into the slot \c cfo_slot_ names, and the noise reformat
  /// reads that same slot back on the DEVICE (see reformat_stage::noise_stage_t::cfo_dev). Reading it
  /// there instead of handing the value over as a kernel parameter is what keeps the scalar out of the
  /// host's hands between the extraction and the weights command buffers - and the host reading it is
  /// what forces it to WAIT for the extraction, which measured 65us of the lane's 211us gap.
  ///
  /// \note Why one slot is not enough, and why this is not theoretical. The estimator instances live in
  /// a pool (one per concurrent PUSCH thread, see concurrent_dependencies in the PUSCH processor) and
  /// an instance is returned to that pool when the processing call ends - while the lane of the hop it
  /// just submitted is still in flight, its completion being deferred. A later hop on the same instance
  /// therefore overwrites the buffer before the earlier hop's K4 has read it, and the kernel then
  /// rotates by a CFO that belongs to another slot. Measured on air: 207 LLR decision flips on one
  /// capture of the device route, intermittently. Rotating the slot past every hop that can be in
  /// flight removes it.
  static constexpr unsigned kCfoSlots = 8;

  /// \brief Rotating slots of the EPRE sum the device reduces from the received pilots (S13-P2).
  ///
  /// Same reason and the same count as kCfoSlots above: the value is written inside the extraction's
  /// command buffer and the HOST reads it much later, when the hop completes - by which time a pooled
  /// instance may have started later hops that overwrite a single destination.
  static constexpr unsigned kEpreSlots = kCfoSlots;

  /// \brief Rotating slots of the device time alignment (K7+K6), one per hop in flight.
  ///
  /// Same reason as the rsrp ring below: the value is written inside the reformat's command buffer and
  /// the HOST reads it much later - after the wait, in complete_fd_td_estimation_stage() - by which
  /// time a pooled estimator instance may have started several later hops.
  ///
  /// One PAGE per slot, not one float: the engine binds the destination with a zero-copy wrap, and a
  /// wrap needs a page-aligned base (a slot at a float offset would be refused and the hop would
  /// silently fall back to the host - see shared_queue::wrap_no_copy).
  static constexpr unsigned kTaSlots       = 8;
  static constexpr unsigned kTaPageFloats  = 4096 / sizeof(float);

  /// \brief Rotating regions of the device rsrp reduction (K5), one region per hop in flight.
  ///
  /// Same reason as kCfoSlots and kSigma2Blocks: the write happens in the reformat's command
  /// buffer, and the HOST reads it much later - after the wait, in complete_fd_td_estimation_stage()
  /// - by which time a pooled estimator instance may have started several later hops that overwrite
  /// a single destination.
  ///
  /// A region is a run of BLOCK SLOTS, kRsrpSlots floats each, addressed by the kernel as
  /// [block slot][layer] -> {sum, count}. A hop needs one slot per standard block plus, when the
  /// batch carries an edge block, ceil(nf_tail / nf_std) more (the merged edge system is strided by
  /// the standard n_blk, see ocudu_mmse_rsrp.metal). The worst shape the estimator produces is
  /// MAX_NOF_BLOCKS standard blocks plus a one-slot edge at MAX_LAYERS layers, which is what the
  /// region is sized for; see rsrp_region_floats().
  static constexpr unsigned kRsrpBlocks = 16;
  static constexpr unsigned kRsrpSlots  = 4;
  float*   gpu_ls_cfo     = nullptr;
  /// The hop's EPRE sum, reduced on the device from the received pilots the extraction kernel built
  /// (see kEpreSlots).
  float*   gpu_ls_epre    = nullptr;
  /// Slot the CURRENT hop uses. Starts one before the first hop so that hop 0 lands on slot 0 and
  /// carries forward from the last (zero-initialised) slot, exactly as the single buffer started at 0.
  unsigned cfo_slot_      = kCfoSlots - 1;
  /// Slot the CURRENT hop's EPRE sum uses. Rotated with the CFO's, one per hop in flight.
  unsigned epre_slot_     = kEpreSlots - 1;
  bool   gpu_nv_ready  = false;

  /// S-7f-5w: the hop's noise variance, computed inside the K0-a command buffer. \c gpu_ls_smoothed
  /// is the frequency-smoothed copy of the pilots the classical estimator reads (a scratch buffer:
  /// the LSE itself must survive for the weights' y vectors), \c gpu_ls_sigma2 is the single float
  /// the kernels leave behind, and \c fd_filter holds the raised-cosine coefficients of the hop's
  /// geometry (a host-side table, see get_fd_smoothing_filter()).
  /// \brief Device rsrp reduction (K5): kRsrpBlocks blocks of kRsrpSlots floats, written by the
  /// reformat's own command buffer when the device stage is on.
  float*                    gpu_rsrp       = nullptr;
  /// Base of THIS hop's block within gpu_rsrp, advanced once per hop.
  unsigned                  rsrp_base_     = 0;
  /// Ring region the hop being STAGED reserved for its own device reduction. defer_unpack() carries
  /// it to the completion, which must not read through rsrp_base_ (see pending_unpack::rsrp_block).
  unsigned                  rsrp_stage_ = 0;
  /// FLOATS that region holds (rsrp_region_floats(), captured with rsrp_stage_).
  unsigned                  rsrp_stage_slots_ = 0;
  /// Whether the device produced this hop's rsrp (so the host must not reduce the pilots again for
  /// a value it will publish).
  bool                      device_rsrp_valid = false;

  /// \brief The device's time alignment (K7+K6), kTaSlots page-aligned slots of one float each.
  ///
  /// \c ta_slot_ is the slot the CURRENT hop reserved. It is an index rather than a pointer because
  /// ::device_ta_s_ below is the value the completion read out of it - and it is read through
  /// get_device_ta_seconds() long after the hop that produced it was staged.
  float*   gpu_ta    = nullptr;
  unsigned ta_slot_  = kTaSlots - 1; // one before the first so that hop 0 lands on slot 0
  /// Whether the device produced this hop's time alignment (so the host must not read the grid back
  /// for a value it will publish).
  bool     device_ta_valid = false;
  /// The value read back at completion, or nullopt when the host must estimate the alignment itself.
  std::optional<float> device_ta_s_{};
  /// \brief SUM of |h|^2 per layer, read out of the device reduction when the hop completed.
  ///
  /// Filled by complete_fd_td_estimation_stage() and answered through get_device_rsrp_sum(), which is
  /// how the published rsrp stops depending on the read-back grid. Only the first nof_layers entries
  /// are meaningful; the array is plain storage so an accessor can hand out a value without a lock.
  std::array<float, 4>      device_rsrp_sums_{};
  float*                    gpu_ls_smoothed = nullptr;
  float*                    gpu_ls_sigma2   = nullptr;
  /// Byte offset (in floats) of THIS hop's sigma2 block within \c gpu_ls_sigma2, which holds
  /// kSigma2Blocks of them. See kSigma2Blocks in the .cpp for why one block is not enough.
  unsigned                  sigma2_base_    = 0;
  std::array<float, 32>     fd_filter{};
  unsigned                  fd_filter_len = 0;
  /// Whether the host asks the device for sigma2 (OCUDU_CE_DEV_SIGMA2=0 keeps the host computation,
  /// which is also the A/B of capture_gates.sh sig2), and whether THIS hop got it.
  bool device_sigma2_enabled = false;
  bool device_sigma2_valid   = false;
  /// \brief BASE of this hop's device noise-variance buffer when the correlation stage may load A's
  /// diagonal from it, or nullptr when it must use the host's ratio (device LSE off or failed,
  /// OCUDU_CE_DEV_SIGMA2=0, a metallib without the power kernel, or OCUDU_CE_K0A_RATIO_DEV=0).
  ///
  /// It is the ONE value the correlation model wants, and both of its consumers take it from the same
  /// place: correlation_stage() hands the base and the slot to the kernels (corr_stage::sigma2_dev /
  /// sigma2_slot) and carries the very same float in corr_stage::sigma2 - the value the host would
  /// have computed - so the matrix flavor, which stages A on the host, and the device builds all load
  /// A's diagonal with one identical float.
  ///
  /// \warning It is the BASE of gpu_ls_sigma2, NOT &gpu_ls_sigma2[kRatioSlot]: the kernel indexes the
  /// base with corr_stage::sigma2_slot, so handing over the element's own address makes the kernel
  /// read past the buffer (that mistake loaded A with no noise loading at all on the first attempt).
  /// The slot is kRatioSlot (see there).
  ///
  /// The device and host ratios really are the same float, and that rests on
  /// ocudu_mmse_pilots.metal being compiled with -fno-fast-math: without it the quotient rounds
  /// differently from the host's in 28.4% of 2^20 pairs, and A's diagonal loading is amplified by
  /// cond_2(A) ~ 2e4 (S-7g-20).
  /// Reset on every hop before K0-a decides, like the other per-hop device state.
  const float* device_sigma2_rel = nullptr;

  /// \brief Whether THIS hop's EPRE sum was reduced by the extraction's command buffer (S13-P2).
  ///
  /// Set with device_ls_valid, and for the same reason: get_device_epre_sum() reads the rotating slot
  /// only for a hop the device actually produced it for, and a hop that failed K0-a must not hand its
  /// reader the previous hop's value.
  bool device_epre_valid = false;

  /// \brief Stages the received DM-RS of the hop - the array the device noise variance reads, and the
  /// same one K4 reads - unless the device builds it for this hop.
  ///
  /// \param[in] device_builds_pilots Whether the extraction kernel produces this hop's received pilots
  ///            (the caller's own device_builds_pilots: same gate, computed once). When true there is
  ///            nothing to stage: the kernel stores what it reads into this very buffer, and the host
  ///            has no copy of them to stage (S13-P2).
  /// \return The number of CDM groups the noise stage can read, or 0 when the geometry does not fit
  ///         the buffers.
  unsigned stage_device_noise_inputs(const fd_td_estimation_stage_args& args, unsigned npt, bool device_builds_pilots);

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
  /// \brief The hop's per-symbol channel estimates, [layer * MAX_NSYMB_PER_SLOT + symbol].
  ///
  /// Mutable because the host copy of the non-DM-RS symbols is materialized on demand, from a const
  /// consumer (see materialize_host_grid()).
  mutable static_re_buffer<MAX_LAYERS * MAX_NSYMB_PER_SLOT, MAX_NOF_SUBCARRIERS> grid_est;

  /// DM-RS symbols of the hop being completed, and how many: the symbols unpack_engine_group()
  /// materializes eagerly, and whether the hop hops (see materialize_host_grid()).
  unsigned unpack_npt     = 0;
  bool     unpack_hopping = false;
  std::array<unsigned, MAX_NOF_DMRS_SYMBOLS> unpack_dmrs_sym{};

  /// Auxiliary enlarged LSE buffers for the sigma2 estimation (RC smoothing).
  static_re_measurement<cf_t, MAX_NOF_PILOTS_SYMBOL, MAX_NOF_DMRS_SYMBOLS, MAX_LAYERS> tmp_lse_enlarged;
  static_re_measurement<cf_t, MAX_NOF_PILOTS_SYMBOL, MAX_NOF_DMRS_SYMBOLS, MAX_LAYERS> tmp_filtered_enlarged;

  /// Symbol-major concatenation of the LSE pilots (input to the statistics provider).
  std::array<cf_t, MAX_NOF_DMRS_SYMBOLS * MAX_NOF_PILOTS_SYMBOL> stats_pilots;

  /// Weight-matrix workspace (R_hp, R_pp + sigma2 I and the inverse, W).
  /// Device-built correlation groups awaiting their slot comparison: at most the standard group and
  /// the edge group of a merged batch.
  static_vector<pending_corr_check, 2>                   pending_corr_checks_;

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
