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

  /// \brief Device-side correlation stage (K0-d): A and R_hp built into the engine's own slots.
  ///
  /// Both matrices are analytic - every element is a time correlation times a frequency
  /// correlation - so the whole product depends on the block geometry (which slot symbols carry
  /// DM-RS, which subcarriers carry pilots) and on the three statistics, and on nothing else: no
  /// received sample, no least-squares estimate, no CFO. The host therefore does not build them at
  /// all: it hands the geometry over and the kernel fills the slots the inversion and the weights
  /// read, which removes the largest CPU block of the estimator ([mmse_time_sum] stage + corr).
  ///
  /// The caller must have zeroed the slots whose pad is expected to be zero (the estimator does,
  /// see stage_engine_group()): the kernel writes the L x L and nout x L blocks only, so a system
  /// laid out with the padded strides keeps the blockdiag(A, I) / [R_hp | 0] structure K1 relies on.
  struct corr_stage {
    /// Destination A slot of the WHOLE batch: [nof_systems][aL_stride][aL_stride] row-major, read
    /// by K1 (in place) or by K3b as the inverse. Only the slot of each system is touched.
    float* a = nullptr;
    /// Destination R_hp slot of the whole batch: [nof_systems][r_stride][aL_stride] row-major.
    float* r_hp = nullptr;
    /// Row stride of the A slots and of BOTH the R_hp rows and columns (>= L). R_hp shares the A
    /// row stride: it is stored row-major as [nout][L] inside a slot laid out as [r_stride][aL_stride].
    unsigned a_l_stride = 0;
    /// Output rows of one R_hp slot (>= nout). Together with a_l_stride it is the slot's footprint
    /// (r_stride * a_l_stride) and therefore the system spacing (see r_sys_stride). It is NOT the
    /// R_hp row stride - that one is a_l_stride.
    unsigned r_stride = 0;
    /// Distance between two systems of the batch, in the A slots and in the R_hp slots. The engine
    /// consumes them in the PACKED order ([sys][L][L], [sys][nout][L]) while the slot's row stride
    /// can belong to another geometry: a merged batch puts a narrower edge block into the standard
    /// slots (a_l_stride = L_std, block order L_e), and stepping by L_e * L_e there would walk into
    /// the standard group. Callers that build one geometry into its own slots leave these at 0 and
    /// the engine uses the packed spacing.
    unsigned a_sys_stride = 0;
    unsigned r_sys_stride = 0;
    /// How many systems this stage covers, starting at its own pointers. 0 means "the whole batch"
    /// (the caller's nof_systems), which is what every single-geometry caller wants.
    ///
    /// A MERGED batch is the case that needs it: it holds the standard blocks in the systems
    /// [0, nof_layers) and the edge block in the systems [nof_layers, 2 * nof_layers), so a prefix
    /// that built this stage's geometry for the BATCH's systems would write the standard matrices
    /// over the edge block's slots - which are a different geometry. That is what the earlier
    /// attempt at a merged device build did (the edge block's slots came out of the standard
    /// geometry, A was then inverted once more by the host's own staging, and a0 read 1861).
    unsigned nof_systems = 0;
    /// Matrix order of one system: npt * npf.
    unsigned l = 0;
    /// Subcarriers of the block (nout = nf * 14).
    unsigned nf = 0;
    /// Pilots per DM-RS symbol and pilots per PRB (comb size) of one system.
    unsigned npf   = 0;
    unsigned ncomb = 0;
    /// Slot symbol period in seconds (1 / (scs_hz * 14)) and subcarrier spacing in hertz.
    float ts     = 0.0F;
    float scs_hz = 0.0F;
    /// Statistics: maximum Doppler shift (time correlation), RMS delay spread (frequency
    /// correlation) and the noise variance that loads A's diagonal.
    float fd_hz      = 0.0F;
    float tau_rms_s  = 0.0F;
    float sigma2     = 0.0F;
    /// DM-RS slot symbols of the hop, ascending (npt entries, at most 4).
    unsigned dmrs_slots[4] = {};
    /// Pilot positions within a PRB, ascending (ncomb entries, at most 12).
    unsigned pilot_re[12] = {};
  };

  /// \brief Builds A and R_hp of every system of \p c on the device.
  ///
  /// The slots are zeroed by the caller (see corr_stage): this writes the L x L and nout x L blocks.
  /// \return True on success; on failure the caller falls back to its own construction.
  bool build_correlation(const corr_stage& c, unsigned nof_systems);

  /// The estimator's INPUT stage of one hop (K0-a): where the pilots come from and where the
  /// least-squares estimates go. Mirrors mmse_pilots_params in ocudu_mmse_pilots.metal.
  struct pilots_stage {
    /// Grid storage as the device view describes it (resource_grid_device_view::base and strides):
    /// already device-resident, so the kernels read it in place - nothing is brought over.
    const void* grid = nullptr;
    /// Whole storage of the grid view, in bytes (the zero-copy mapping needs the extent).
    std::size_t grid_bytes = 0;
    unsigned    grid_subc_stride = 0;
    unsigned    grid_symb_stride = 0;
    unsigned    grid_port_stride = 0;
    /// Transmitted DM-RS of the hop, [symbol][layer][pilot] real/imag interleaved (host array).
    const float* ref = nullptr;
    /// CAPACITY of the ref and lse buffers, in bytes - not this hop's used length.
    ///
    /// The engine maps a host pointer to a Metal buffer through a pointer-keyed cache, and it warns
    /// and re-maps whenever a request is LARGER than the entry already cached for that pointer. The
    /// pilot count varies from hop to hop with the allocation width, so wrapping the per-hop length
    /// made a fixed buffer look like a growing one: measured on air, 15580 re-maps in one two-minute
    /// leg (0 before K0-a). Always wrap the whole allocation.
    std::size_t buf_bytes = 0;
    /// Symbol start times of the slot (needed by the CFO phasors).
    const float* epochs = nullptr;
    /// Destination of the least-squares pilots, [symbol][layer][pilot] real/imag interleaved.
    float* lse = nullptr;
    /// Destination of the estimated CFO (a single float).
    float* cfo = nullptr;
    /// Received DM-RS of the hop, [symbol][cdm][pilot] real/imag interleaved - the same array the
    /// equalizer's noise reduction (K4) reads. The noise variance below needs it.
    const float* rx_pilots = nullptr;
    /// CAPACITY of \c rx_pilots in bytes.
    std::size_t rx_bytes = 0;
    /// Destination of the frequency-smoothed pilots, [symbol][layer][pilot] real/imag interleaved.
    /// A scratch buffer of the same size as \c lse: the smoothing is what the noise variance is
    /// estimated from, and the LSE itself must survive (the weights' y vectors are built from it
    /// later, in the engine's own command buffer).
    float* smoothed = nullptr;
    /// Destination of the hop's noise variance and of the pilots' power sum (TWO floats: [0] sigma2,
    /// [1] the sum of |LS pilot|^2), or nullptr to skip both. When set, the extra dispatches below
    /// ride THIS command buffer, so the host reads two scalars after the wait instead of running
    /// estimate_sigma2() (measured 3.3 us per hop of host time on air) and of walking the pilots for
    /// their mean power. The power sum is skipped - and left untouched - whenever this whole block is
    /// (see sigma2_done).
    float* sigma2 = nullptr;
    /// \brief Set by build_pilots_lse() to whether it actually encoded the noise-variance stage.
    ///
    /// A caller that asked for \c sigma2 must test THIS, not the pointer it passed in. The stage is
    /// skipped - and the destination left untouched - when the hop's geometry exceeds the kernels'
    /// compile-time maxima (ocudu_metal_mmse_engine.mm refuses those instead of letting the kernels
    /// clamp them, which would truncate silently), and the build itself still succeeds because the
    /// least-squares pilots are unaffected. Without the flag the caller could only see a non-null
    /// \c sigma2 and would read back a value the device never wrote.
    bool* sigma2_done = nullptr;
    /// Raised-cosine coefficients of the frequency-domain smoothing (a host-side table, geometry
    /// only) and how many virtual pilots each edge takes. They belong to the smoothing alone.
    const float* fd_filter = nullptr;
    /// CAPACITY of \c fd_filter in bytes - not this hop's filter length.
    ///
    /// Same reason as \c buf_bytes: the filter is a fixed host array whose used length follows the
    /// grant width (11 / 21 / 31 for 1 / 2 / 3 or more allocated PRB), so wrapping the per-hop
    /// length makes one array look like a growing buffer as soon as a wide grant follows a narrow
    /// one, and the engine re-wraps it (a warning, plus a new Metal object over the same memory).
    std::size_t fd_filter_bytes = 0;
    unsigned    fd_filter_len   = 0;
    unsigned    nof_v_pilots    = 0;
    /// CDM groups of the hop (the received pilots are indexed by group, the layers are paired).
    unsigned nof_cdm = 0;
    /// DM-RS to data amplitude scaling (the classical noise estimator scales by beta / nof_dmrs_symb),
    /// and whether the CFO is compensated in the pilots the noise is estimated from.
    float    beta           = 1.0F;
    /// 1 / beta: the smoothing below has to reproduce the host's, which smooths the pilots the caller
    /// has ALREADY scaled by this (estimate_sigma2(): "The caller has already applied the DM-RS to
    /// data scaling (1 / beta) to the LSE pilots"). The device reads the unscaled LSE.
    float    inv_beta       = 1.0F;
    bool     compensate_cfo = false;
    unsigned nof_dmrs_symb = 0;
    unsigned nof_layers    = 0;
    unsigned nof_pilots    = 0;
    unsigned ncomb         = 0;
    unsigned nof_prb       = 0;
    unsigned first_prb     = 0;
    unsigned port          = 0;
    /// Slot symbol index of each hop DM-RS symbol, in hop order (at most 4).
    unsigned dmrs_symb[4] = {};
    /// Pilot positions within a PRB, ascending (at most 12).
    unsigned pilot_re[12] = {};
  };

  /// \brief Runs the estimator's input stage (K0-a) on the device: pilot extraction from the
  /// device-resident grid, least-squares estimates, CFO estimation and compensation.
  ///
  /// The host consumes \c lse right afterwards (it is the estimator's input), so this completes
  /// synchronously - the asynchronous form is a separate step (see the plan).
  /// \return True on success; on failure the caller keeps its own host pre-stage.
  bool build_pilots_lse(const pilots_stage& s);

  /// The engine's pilot vectors (y) as the DEVICE writes them - glue #2 of the plan (S-7f-5u).
  ///
  /// \c build_pilots_lse() produces the hop layout ([symbol][layer][pilot]) while K2 consumes a
  /// block layout, so something has to convert. The host used to, in two memcpys per group per hop
  /// (device -> pilots_lse_view -> y slots). This descriptor hands the conversion to the device:
  /// the kernel re-indexes the device's own output into the y slots, and the host never touches
  /// them.
  ///
  /// One descriptor per STAGED GROUP, not per hop: a merged batch stages the standard blocks and
  /// the narrower edge block as two groups and encodes both descriptors into the one command
  /// buffer it already commits (so the command buffers per hop do not move).
  ///
  /// The values are byte-identical to the host's staging - the kernel re-indexes and applies the
  /// same single-precision product the host applies - so OCUDU_CE_DEV_Y=0, which keeps the host
  /// staging, is an exact A/B and the gate for this path.
  struct pilots_scatter {
    /// Source: the least-squares pilots of the hop, [symb][layer][pilot] real/imag interleaved -
    /// the HOP layout build_pilots_lse() wrote, i.e. pilots_stage::lse.
    const float* lse = nullptr;
    /// CAPACITY of the source in bytes (the zero-copy cache is pointer-keyed: a larger request
    /// re-wraps, see pilots_stage::buf_bytes).
    std::size_t lse_bytes = 0;
    /// Destination: this group's y slots, [layer][n_blk_slots][2 * Ls] real/imag interleaved, with
    /// the group's system offset already in the pointer. The engine does NOT bind this pointer
    /// directly: it turns it into an OFFSET into the y buffer the batch call binds (the y argument
    /// of run_async()/run()), because a second MTLBuffer object over the same memory is a DIFFERENT
    /// resource to Metal. Binding the tail group through its own wrap left its scatter unordered
    /// with respect to K2's read of it - measured: the merged tail group came out as the host
    /// memset's zeros and the split tail as a previous submission's leftovers, while the standard
    /// group, whose pointer IS the batch base, was correct.
    float* y = nullptr;
    /// Geometry of the group (see mmse_scatter_params in ocudu_mmse_pilots.metal, field by field).
    unsigned nof_layers  = 0;
    unsigned nof_symb    = 0;
    unsigned nof_pilots  = 0;
    unsigned npf         = 0;
    unsigned pilot_base  = 0;
    unsigned n_blk_slots = 0;
    unsigned n_blk_real  = 0;
    unsigned Ls          = 0;
    /// DM-RS to data scaling the host applies to its own pilots (1 / beta_scaling).
    float inv_beta = 1.0F;
  };

  /// Whether the device-side pilot scatter (glue #2) can run: the metallib carries
  /// mmse_pilots_scatter_y. When false the caller keeps staging y on the host.
  bool scatter_available() const;

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
  /// \param scatter Optional glue #2 stage encoded FIRST in the same command buffer (see
  ///                run_async()): the device writes the y slots this call is about to read.
  bool run(float* a, const float* r_hp, float* w, const float* y, float* h, unsigned nout, unsigned L,
           unsigned nof_systems, unsigned nof_blocks, const reformat_stage* reformat = nullptr,
           const pilots_scatter* scatter = nullptr, unsigned nof_scatter = 0);

  /// \brief Hot path (v1): the weights (K2) with a pre-inverted A, in ONE command buffer.
  /// The A^-1 inversion itself is the CALLER's: it either runs the host Gauss-Jordan or has the
  /// device build the matrices and inverts those on the host (see corr below and
  /// build_correlation()). See run() for the entry point that has K1 invert A in this same buffer.
  /// \param[in] reformat Optional K3 stage appended to the same command buffer (see run()).
  /// \param[in] corr     Optional K0-d stage PREPENDED to the same command buffer: the correlation
  ///                     matrices are built where the weights read them, and K1 (dispatched right
  ///                     after, in this same buffer) inverts them in place. That keeps the whole
  ///                     batch on the device, at the price of K1's accuracy - usable up to the
  ///                     block order its elimination is exact at (36). For the order this hardware
  ///                     runs (54) build the matrices with build_correlation(), invert them on the
  ///                     host, and call this entry point with corr == nullptr instead (which is what
  ///                     the estimator does).
  bool run_weights_only(const float* a_inv, const float* r_hp, float* w, const float* y, float* h, unsigned nout,
                        unsigned L, unsigned nof_systems, unsigned nof_blocks,
                        const reformat_stage* reformat = nullptr, const corr_stage* corr = nullptr,
                        const pilots_scatter* scatter = nullptr, unsigned nof_scatter = 0);

  /// \brief As run_weights_only(), but commits WITHOUT waiting for the GPU.
  ///
  /// The weights-only pipeline (the caller inverted A on the host, so K1 is not part of the batch) is the
  /// path every hop with a block order above the inversion kernel's limit takes, and it used to be the one
  /// path that could not defer: the estimator had to wait for the whole batch inside its stage, which at the
  /// OTA geometry (block order 54, limit 36) cost ~150-230us per hop of pure waiting. With this entry point
  /// the batch defers like the inverted one: the wait moves to wait_pending(), which the consumer calls once
  /// it has nothing else to do (the receiving chain's demodulation runs inside that window).
  ///
  /// \return False when the encoding failed and nothing was submitted.
  bool run_weights_only_async(const float* a_inv,
                              const float* r_hp,
                              float*       w,
                              const float* y,
                              float*       h,
                              unsigned     nout,
                              unsigned     L,
                              unsigned     nof_systems,
                              unsigned     nof_blocks,
                              const reformat_stage* reformat = nullptr,
                              const corr_stage*     corr     = nullptr,
                              const pilots_scatter* scatter  = nullptr,
                              unsigned              nof_scatter = 0);

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
  /// \param corr When non-null, the correlation build is encoded as a PREFIX of this same command
  ///             buffer, before K1. That is only correct when the SLOTS ARE LEFT HOLDING A (the
  ///             device-inversion route): the prefix writes A and R_hp on the device, and the host
  ///             does not touch either afterwards. When the host is the inverter it has to read the
  ///             device's A between the two, so it keeps calling build_correlation() standalone.
  /// \param scatter When non-null, \p nof_scatter pilot scatters are encoded FIRST in this buffer
  ///             (before K2 reads y). The caller MUST have left the y slots alone: the device is
  ///             their only writer on that route, exactly as with the correlation prefix. Encoding
  ///             failure is reported like every other failure here - nothing is committed, the
  ///             caller falls back - but note the caller's fallback must not read y, because the
  ///             host did not stage it (the CPU block path does not).
  bool run_async(float*       a,
                 const float* r_hp,
                 float*       w,
                 const float* y,
                 float*       h,
                 unsigned     nout,
                 unsigned     L,
                 unsigned     nof_systems,
                 unsigned     nof_blocks,
                 const reformat_stage* reformat    = nullptr,
                 const corr_stage*     corr        = nullptr,
                 const pilots_scatter* scatter     = nullptr,
                 unsigned              nof_scatter = 0);

  /// \brief Encodes this engine's dispatches into the shared burst of the deferred chain.
  ///
  /// Off (the default): every stage opens, commits and waits its own command buffer - the synchronous
  /// contract this engine was built with, and what the callers that read the estimates on the host need.
  ///
  /// On: the stages of the deferred PUSCH chain encode into the burst the equalizer and the demapper share
  /// (see shared_burst), the CE -> equalizer order comes from the barrier the burst inserts when the pipeline
  /// changes, and no stage commits or waits: the lane's single commit covers the estimator too, which removes
  /// one of the two host waits a lane used to pay ([mmse_time_sum] gpu_wait).
  ///
  /// \param[in] enabled Whether the following stages encode into the shared burst.
  /// \note Only sound while the caller owns a burst: without one, begin_stage() falls back to the stage's own
  ///       command buffer - a slow lane, never a wrong one.
  void set_fused_burst(bool enabled);

  /// \brief Waits for the submission of run_async() and reports whether it completed.
  /// \return True when there was nothing pending, or when the pending submission succeeded.
  bool wait_pending();

  /// \brief Completes the dispatches a burst-mode stage left in the shared burst, and reports success.
  ///
  /// A burst-mode stage (set_fused_burst(true)) leaves its dispatches in the command buffer the rest of
  /// the receiving chain shares, so that the lane's own commit and wait cover them: by the time the
  /// estimator is asked to complete the hop, this call normally finds nothing outstanding and returns at
  /// once. The callers that read the hop's results BEFORE the lane commits are the ones that need it to do
  /// the work - the demodulator on the route that syncs the estimates to host memory (OCUDU_CE_CPU_CE),
  /// which completes the estimation before it submits the equalization, and the debug capture. The burst
  /// then holds this engine's dispatches alone (no other stage has encoded into it yet), so committing it
  /// here gives up the overlap, never the ordering the burst exists for.
  ///
  /// \note This is the burst-mode counterpart of wait_pending(): that one completes the engine's own command
  ///       buffer, this one the shared burst, and the engine has nothing of its own pending in burst mode.
  ///       The caller knows which one it left behind (see
  ///       port_channel_estimator_metal_mmse_impl::pending_fused_burst).
  /// \return True when everything the calling thread's lane holds completed successfully.
  bool complete_fused_burst();

  /// \brief Diagnostics: whether the calling thread's receiving chain has a burst open, and how many
  /// dispatches it holds.
  ///
  /// The burst is thread local and shared by the stages of one lane (see shared_burst), so these answer
  /// for the CALLING THREAD rather than for this engine instance. That is what makes them usable from
  /// outside the Objective-C++ layer - the estimator's unit test checks that a fused hop really handed
  /// its dispatches over, and that a synchronous one did not, because a comparison of the published
  /// values alone cannot tell a route that works from a knob that never arrived.
  static bool     burst_is_open();
  static unsigned burst_dispatch_count();

  /// \brief Diagnostics: the receiving chain's own synchronization point - commit the open burst (if any)
  /// and wait for everything this thread's lane holds.
  ///
  /// This is what the lane does when it collects a group of demodulated symbols; the unit test uses it to
  /// put the estimator's completion in the order the air path has (lane first, completion after), instead
  /// of only in the order the host-read route has (completion first).
  static bool burst_commit_and_wait();

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
