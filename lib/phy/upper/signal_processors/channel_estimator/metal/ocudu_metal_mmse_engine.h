// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief ocudu-side Metal engine for the 2D MMSE channel estimator.
///
/// Thin Objective-C++ implementation (see ocudu_metal_mmse_engine.mm) that loads the
/// pre-compiled .metallib shader libraries and executes the two MMSE core operators:
///   K1  ocudu_mmse_inv.metal   : batched Gauss-Jordan inversion of LxL matrices
///                                (one threadgroup per system).
///   K2  ocudu_mmse_apply.metal : batched W . y block matrix multiplications
///                                (one threadgroup per (system, block)).
/// The engine follows the LDPC Metal engine pattern: zero-copy buffers
/// (newBufferWithBytesNoCopy, 4KB alignment), synchronous waitUntilCompleted and
/// last_gpu_wait_us() GPU-side timing.

#pragma once

#include "ocudu/ran/pusch/pusch_constants.h"
#include <cstddef>
#include <cstdint>

namespace ocudu {
namespace metal {

/// Synchronous single-instance MMSE compute engine.
class mmse_engine
{
public:
  /// Maximum number of systems per slot (ports x layers x hops).
  static constexpr unsigned MAX_SYSTEMS = 8;

  mmse_engine()  = default;
  ~mmse_engine();

  mmse_engine(const mmse_engine&)            = delete;
  mmse_engine& operator=(const mmse_engine&) = delete;

  /// \brief One-shot setup: device/queue/pipelines and .metallib loading.
  ///
  /// \param[in] metallib_path Path to the pre-compiled shader library (may be null to use
  ///                          the baked-in default path).
  /// \return True on success.
  bool init(const char* metallib_path = nullptr);

  /// \brief Device-side reformat stage (K3) of the per-hop pipeline: the equalizer's channel
  /// estimates, built from the K2 output inside the same command buffer.
  ///
  /// The equalizer consumes, per OFDM symbol and layer, the channel estimates of the allocated
  /// data resource elements packed in ascending subcarrier order as cbf16 (see
  /// channel_equalizer::ch_est_list). K3 gathers them out of the block layout K2 writes, applying
  /// the per-symbol RE mask, so the host never walks the grid to build that input.
  struct reformat_stage {
    /// Destination: [nof_layers][total_re] complex cbf16 (2 x uint16 per element), resident in
    /// the estimator's staging buffers.
    void* dst = nullptr;
    /// Prefix RE counts: offsets[s] is the destination index of the first RE of symbol s and
    /// offsets[nof_symbols] == total_re. Only the first nof_symbols + 1 entries are read.
    const uint32_t* offsets = nullptr;
    unsigned nof_symbols    = 0;
    unsigned total_re       = 0;
    /// Data REs per PRB of a symbol without (drpp) and with (drpp_dmrs) DM-RS, the DM-RS RE
    /// positions within a PRB (12 bits) and the DM-RS symbols of the slot (one bit per symbol).
    /// The destination index is derived from these arithmetically: the allocation is contiguous,
    /// so every PRB of a symbol contributes the same data REs.
    unsigned drpp          = 0;
    unsigned drpp_dmrs     = 0;
    unsigned dmrs_re_bits  = 0;
    unsigned dmrs_sym_bits = 0;
    /// Destination layers (the systems of the batch hold either one block geometry per layer, or
    /// two: the standard blocks in [0, nof_layers) and the edge block in [sys_tail, ...)).
    unsigned nof_layers = 0;
    /// Subcarriers of one standard block (the batch's nof_blocks of them cover
    /// [0, nf_std * nof_blocks)) and of the edge block, at the systems [sys_tail, ...), block 0.
    unsigned nf_std   = 0;
    unsigned nf_tail  = 0;
    unsigned sys_tail = 0;
    bool     has_tail = false;
    /// DC subcarrier of the allocation, relative to its first subcarrier (>= nf_std * n_blk +
    /// nf_tail when there is none): the gather writes a zero estimate there.
    unsigned dc_sc = ~0u;

    /// \brief Optional K4 stage: the noise variance the equalizer scales its soft bits with,
    /// reduced from the same h into a device value, so that no consumer of the estimate has to
    /// read the grid before dispatching the equalizer.
    struct noise_stage_t {
      /// Destination: one float, written by the reduction.
      float* nv = nullptr;
      /// Transmitted pilots, [npt][nof_layers][npf] complex, staged by the estimator.
      const void* pilots = nullptr;
      /// Received pilots, [npt][nof_cdm_groups][npf] complex, staged by the estimator.
      const void* rx_pilots = nullptr;
      /// Start time of every slot symbol, in symbol durations (MAX_NSYMB_PER_SLOT entries).
      const float* symbol_start_epochs = nullptr;
      unsigned     npt                 = 0;
      unsigned     nof_cdm_groups      = 0;
      unsigned     npf                 = 0;
      /// Slot symbols carrying DM-RS in this hop, ascending. The kernel's parameter block hard-codes
      /// this size, so it must match the estimator's MAX_DMRS_SYMBOLS (4) exactly.
      unsigned dmrs_slots[4] = {};
      unsigned nof_prb                      = 0;
      unsigned comb_size                    = 0;
      unsigned dmrs_re_bits                 = 0;
      float    beta                         = 1.0F;
      float    cfo                          = 0.0F;
      bool     compensate_cfo               = false;
      /// Pilots of the hop (all its DM-RS symbols and layers) and the CDM groups of the
      /// transmission, i.e. the sample count the host normalizes the variance by, plus the SINR
      /// ceiling it bounds it with.
      unsigned nof_dmrs_pilots = 0;
      unsigned nof_cdm         = 0;
      float    min_snr_power   = 1.0F;
    } noise;
  };

  /// \brief Batched inversion (K1): A_inv = (A)^-1 for each system, in-place Gauss-Jordan.
  ///
  /// \param[in,out] a           [systems][n][n] row-major matrices (overwritten with the inverse).
  /// \param[in]     n           Matrix order (<= 36).
  /// \param[in]     nof_systems Number of systems (<= MAX_SYSTEMS).
  /// \return True on success.
  bool invert(float* a, unsigned n, unsigned nof_systems);

  /// \brief Batched block matrix multiplication (K2): h = W . y for each (system, block).
  ///
  /// \param[in]  w           [systems][nout][L] row-major real weights.
  /// \param[in]  y           [systems][nof_blocks][L] real/imag interleaved pilot vectors.
  /// \param[out] h           [systems][nof_blocks][nout] real/imag interleaved outputs.
  /// \param[in]  nout        Number of block output positions (<= 504).
  /// \param[in]  L           Number of block pilots (<= 36).
  /// \param[in]  nof_systems Number of systems (<= MAX_SYSTEMS).
  /// \param[in]  nof_blocks  Number of time-frequency blocks.
  /// \return True on success.
  bool apply(const float* w, const float* y, float* h, unsigned nout, unsigned L, unsigned nof_systems,
             unsigned nof_blocks);

  /// \brief Combined K1 + K1b + K2 pipeline in ONE command buffer (single commit/wait):
  /// A^-1 (in place), W = R_hp . A^-1, h = W . y. This is the per-slot hot path.
  ///
  /// \param[in,out] a           [systems][L][L] matrices (overwritten with the inverse).
  /// \param[in]     r_hp        [systems][nout][L] row-major cross-correlation matrices.
  /// \param[out]    w           [systems][nout][L] weight matrices.
  /// \param[in]     y           [systems][nof_blocks][2L] real/imag interleaved pilots.
  /// \param[out]    h           [systems][nof_blocks][2nout] real/imag interleaved outputs.
  /// \param[in]     nout        Number of block output positions (<= 504).
  /// \param[in]     L           Number of block pilots (<= 36).
  /// \param[in]     nof_systems Number of systems (<= MAX_SYSTEMS).
  /// \param[in]     nof_blocks  Number of time-frequency blocks.
  /// \param[in]     reformat    Optional K3 stage appended to the same command buffer, turning the
  ///                            h it just produced into the equalizer's per-symbol estimates. Pass
  ///                            nullptr (the default) to skip it.
  /// \return True on success.
  bool run(float* a, const float* r_hp, float* w, const float* y, float* h, unsigned nout, unsigned L,
           unsigned nof_systems, unsigned nof_blocks, const reformat_stage* reformat = nullptr);

  /// \brief Hot path (v1): K1b + K2 in ONE command buffer with a single commit/wait.
  /// The A^-1 inversion runs on the CPU (the batched 36x36 Gauss-Jordan kernel is
  /// barrier-bound on the GPU - see PLAN.md 7.0.6); the Metal inversion kernel remains
  /// as the algorithm skeleton and the golden reference.
  /// \param[in] reformat Optional K3 stage appended to the same command buffer (see run()).
  bool run_weights_only(const float* a_inv, const float* r_hp, float* w, const float* y, float* h, unsigned nout,
                        unsigned L, unsigned nof_systems, unsigned nof_blocks,
                        const reformat_stage* reformat = nullptr);

  /// \brief Compiles the simdgroup_matrix 8x8 pipelines of the metal_nn_mmse variant
  /// (mmse_weights_matrix / mmse_apply_matrix, ocudu_mmse_*_matrix.metal).
  ///
  /// Call after init(); idempotent. Returns false when the loaded .metallib does not
  /// contain the matrix kernels (e.g. it predates this feature) - the caller then keeps
  /// the legacy run_weights_only()/CPU path as the automatic fallback.
  bool init_matrix_pipelines();

  /// \brief A/B twin of run_weights_only() on the GPU hardware matrix unit:
  /// W = R_hp . A^-1 (mmse_weights_matrix) and h = W . Y (mmse_apply_matrix) in ONE
  /// command buffer. The kernels ALWAYS run on the hardware matrix unit: any nout/L are
  /// zero-padded by the caller to ceil8 (Np/Lp) inside the staging buffers and the apply
  /// kernel truncates the output back to the real nout - see the *_matrix.metal comments
  /// for the padding contract. Only a zero dimension batch returns false (no GPU work).
  ///
  /// Staging layouts (all pad rows/columns beyond the real nout/L must be zero):
  /// \param[in]  a_inv       [systems][Lp][Lp] row-major inverted matrices, Lp = ceil8(L)
  ///                         (CPU Gauss-Jordan result in rows/cols < L, zero elsewhere).
  /// \param[in]  r_hp        [systems][Np][Lp] row-major cross-correlation matrices,
  ///                         Np = ceil8(nout), real values in rows < nout and cols < L.
  /// \param[out] w           [systems][Np][Lp] weight matrices (written fully; the pad
  ///                         columns/rows come out exactly zero).
  /// \param[in]  qy          [systems][ceil(nof_blocks/4)][Lp][8] row-major REAL pilot
  ///                         matrix packed by the caller: row k = block pilot (symbol-major),
  ///                         col 2*(block%4)+{0,1} = {real,imag} of that block; rows >= L
  ///                         and the columns of the non-existent tail-quad blocks must be 0.
  /// \param[out] h           [systems][nof_blocks][2*nout] real/imag interleaved outputs
  ///                         (identical layout to run_weights_only; padded rows dropped).
  /// \return True on success.
  bool run_nn(const float* a_inv,
              const float* r_hp,
              float*       w,
              const float* qy,
              float*       h,
              unsigned     nout,
              unsigned     L,
              unsigned     nof_systems,
              unsigned     nof_blocks);

  /// \brief Encodes the combined pipeline and commits it WITHOUT waiting for the GPU.
  ///
  /// The estimator uses this to overlap the GPU work with the host-side preparation of whatever
  /// consumes it: the wait moves to wait_pending(), which the consumer calls once it has nothing
  /// else to do. Any submission still outstanding when a run*() entry point is called is waited for
  /// first, so an engine never holds more than one command buffer in flight - this is deliberately
  /// NOT the batch API that stalled in the field (many command buffers committed late, queue slots
  /// exhausted); here every call commits immediately and at most one is outstanding.
  ///
  /// \return False when the encoding failed and nothing was submitted.
  bool run_async(float*       a,
                 const float* r_hp,
                 float*       w,
                 const float* y,
                 float*       h,
                 unsigned     nout,
                 unsigned     L,
                 unsigned     nof_systems,
                 unsigned     nof_blocks,
                 const reformat_stage* reformat = nullptr);

  /// \brief Waits for the submission of run_async() and reports whether it completed.
  /// \return True when there was nothing pending, or when the pending submission succeeded.
  bool wait_pending();

  /// Whether a submission from run_async() is still outstanding.
  bool has_pending() const;

  /// \brief Reserves the zero-copy mapping of a buffer at its maximum size.
  ///
  /// The zero-copy cache is keyed by pointer and keeps the mapping created first: a later request
  /// for the same pointer with a LARGER size re-wraps (a new Metal buffer, and the cache keeps the
  /// old entry), while a smaller one is served from the cache. A buffer whose size follows the
  /// allocation - the estimator's staging buffers do - must therefore be reserved at its capacity
  /// once, or every hop that needs more than the first one allocated so far creates a Metal buffer
  /// on the hot path (measured: 6492 re-wraps in one 160 s run, each with a warning line).
  /// \return True when the mapping exists.
  bool reserve_buffer(const void* ptr, std::size_t bytes);

  /// \brief Reserves the zero-copy mapping of a buffer that another engine consumes.
  ///
  /// The estimator's own staging buffers are internal: only this engine binds them, so an
  /// engine-private mapping is enough (reserve_buffer()). The tensors a later stage of the chain
  /// reads - the K3 estimates and the K4 noise variance, both of which the equalizer binds - are
  /// not: Metal only relates the accesses of two dispatches through the resource they are bound to,
  /// so the producing stage and the consuming stage must bind the same Metal buffer object. Those
  /// buffers are mapped in the process-wide cache that every Metal engine shares
  /// (metal::shared_queue::wrap_no_copy), which is keyed by pointer and hands the same object to
  /// every caller.
  ///
  /// \note The shared mapping rounds the length up to a whole page, so an exported buffer must own
  /// a whole number of pages - the estimator allocates them page-rounded for this reason.
  /// \return True when the mapping exists.
  bool reserve_shared_buffer(const void* ptr, std::size_t bytes);

  /// Returns the GPU-side duration of the last operation in microseconds (0 when unavailable).
  double last_gpu_wait_us() const;

private:
  void* impl = nullptr;
};

} // namespace metal
} // namespace ocudu
