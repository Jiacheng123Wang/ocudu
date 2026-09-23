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
#include <array>
#include <cstddef>
#include <cstdint>

namespace ocudu {
namespace metal {

/// \brief Where the estimator's deferred hop puts its dispatches, and how the lane is ordered after them.
///
/// The receiving chain encodes the estimation of a hop, then the equalization and the demapping of the
/// same group, and the equalizer reads what the estimation wrote (the weights, the per-symbol estimates,
/// the noise variance). Three ways to get that order, and they differ in WHEN the estimator's command
/// buffer is committed - which is what decides whether its GPU work overlaps the host encoding the rest
/// of the lane:
///
///  * \c event      - the estimator's own command buffer, committed as soon as its dispatches are
///                    encoded, with the lane burst waiting on it through the back-end stage fence
///                    (shared_queue::backend_stage_wait()). Overlap AND ordering, no host wait: this is
///                    the default (S-7g-19, Step 1').
///  * \c host_wait  - the same own command buffer, waited for by the host before the lane is encoded.
///                    The historical route: correct, and the reason the lane used to pay ~125us of
///                    [ul_equalization_demod] (the host blocked while the estimator's GPU work ran
///                    instead of encoding the equalization). Kept as the escape hatch.
///  * \c burst      - the estimator's dispatches join the lane's shared command buffer (S-7g-16, Step
///                    1b): one submission for the whole lane, at the price of the estimator's GPU work
///                    no longer overlapping the host's encoding (the +125us debt Step 1' came to pay
///                    back) and of the estimator's own command buffer not existing at all.
///  * \c merged     - S13-P2/P3: the EXTRACTION opens the hop's command buffer and holds it
///                    (pilots_stage::hold_for_weights), the weights continue in it (a second encoder),
///                    and the lane's own stages (equalization, demapping) continue in it as well
///                    (shared_burst::adopt()). One submission carries the whole hop, committed and
///                    waited by the lane. It pays the same non-overlap price as \c burst, and it needs
///                    the caller's promise that nothing on the host reads the extraction's results
///                    before the lane commits - which is what pilots_stage::hold_for_weights states.
///
/// \note All four orders are byte-identical by construction; the unit test compares them on the same
///       input, and the air legs judge which one is faster.
enum class ce_lane_order { event, host_wait, burst, merged };

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

    /// \brief Optional K5 stage: the hop's per-layer rsrp, reduced on the DEVICE from the same h.
    ///
    /// The reporting values (rsrp, and the SNIR the CSI report derives from it) are the only reason
    /// the host still reads the estimated grid back after every hop. This stage computes them where
    /// the estimates already are, so the host reads one float per layer instead of the grid.
    ///
    /// It reads h - the reformat's SOURCE, not its destination - because K3 deliberately EXCLUDES the
    /// pilot resource elements from its layout (they carry no data and the equalizer does not index
    /// by them), so the pilots are reachable in h and nowhere in dst. The geometry it needs is the
    /// reformat's own, reused field for field so the two cannot disagree about which subcarrier is
    /// which.
    struct rsrp_stage_t {
      /// Destination: [n_blk][nof_layers] floats, RAW sums of |h|^2 over the hop's pilot REs.
      /// The host applies the normalization, so the two reductions stay comparable term by term.
      float* dst = nullptr;
      /// Number of block slots the batch carries (the reformat's own nof_blocks).
      unsigned n_blk = 0;
      /// The layer's own pilot comb within a PRB, one 12-bit mask per layer, in [0, nof_layers).
      /// NOT the union the reformat uses for its "is this RE a pilot" test: a two-layer hop has two
      /// combs and each layer's rsrp is reduced over its OWN pilots.
      /// MAX_LAYERS (4) entries; the kernel's parameter block hard-codes that size, so it
      /// must stay in step with the estimator's own MAX_LAYERS.
      static constexpr unsigned      max_layers = 4;
      std::array<unsigned, max_layers> pilot_re_bits{};
    };

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
      /// \brief Start epoch of each of the hop's DM-RS symbols, in symbol durations (npt entries used).
      ///
      /// Batch 5g: these replaced the 14-float array the host used to upload into a device buffer -
      /// the lane's last host -> device write. They travel as kernel parameters because THIS kernel's
      /// answer is a reduction and must not be given new arithmetic to compile around: deriving them
      /// here moved the published noise variance by 2 ulp on 3 of 27 captures (measured; see
      /// mmse_noise_params::dmrs_epochs in ocudu_mmse_reformat.metal). K0-a's CFO kernels derive the
      /// same values on the device instead - see ocudu_mmse_epochs.h.
      float    dmrs_epochs[4] = {};
      unsigned npt            = 0;
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
      /// \brief Where the rotation's CFO comes from.
      ///
      /// The hop's CFO is produced in one of two places and the kernel has to be told which. When the
      /// DEVICE built the pilots (K0-a), its own estimate is written into \c cfo_dev inside the
      /// extraction's command buffer, and this stage - ordered after it - reads it there; when the host
      /// pre-stage built them instead (the cold path, or a route without the device extraction), the
      /// host's answer is the matching one and travels as the \c cfo parameter.
      ///
      /// \note Why the device's value is not simply read back into \c cfo, which is what this replaces:
      /// that read cannot happen until the extraction's command buffer has COMPLETED, so the host had
      /// to wait for it before it could encode this one - and the device then sat idle for the rest of
      /// that hand-over (measured on air: 65us of a 211us lane gap, of which ~45us is the driver's
      /// completion wake-up and only ~20us is host work). \c cfo_dev points at the rotating slot THIS
      /// hop reserved (see port_channel_estimator_metal_mmse_impl.h), so a later hop on the same pooled
      /// estimator instance cannot overwrite it before this read.
      const float* cfo_dev         = nullptr;
      bool         cfo_from_device = false;
      /// Pilots of the hop (all its DM-RS symbols and layers) and the CDM groups of the
      /// transmission, i.e. the sample count the host normalizes the variance by, plus the SINR
      /// ceiling it bounds it with.
      unsigned nof_dmrs_pilots = 0;
      unsigned nof_cdm         = 0;
      float    min_snr_power   = 1.0F;
    } noise;

    /// See rsrp_stage_t: the hop's per-layer rsrp, reduced on the device from this reformat's own h.
    /// Left empty (dst == nullptr) when the caller wants the host to keep computing it, which is what
    /// OCUDU_CE_DEV_STATS=0 asks for.
    rsrp_stage_t rsrp;

    /// \brief Optional batch-5b stage: the hop's time alignment, reduced on the DEVICE.
    ///
    /// The host derives the hop's timing advance from the pilot estimates in three steps - an inverse
    /// transform per DM-RS symbol/layer, the accumulation of their |.|^2, and a peak search in half a
    /// cyclic prefix either side of the resulting profile - and this stage runs all three where the
    /// estimates already are: K7 places them into a transform input, the DFT kernel transforms each
    /// slice, K6 reduces the profile to seconds. All three dispatches go into the command buffer this
    /// stage is encoded into, so the host never waits for an intermediate value.
    ///
    /// The input is the reformat's OWN \c h (the buffer K5 reads), not a separate staging: the host's
    /// TA input is filled out of a host copy of that very buffer (see ocudu_mmse_ta.metal), so reading
    /// it on the device is what makes the two sides agree RE for RE.
    struct ta_stage_t {
      /// Destination: one float, the time alignment in SECONDS. It must be page-aligned: it is bound
      /// with a zero-copy wrap (a caller's stack float is not, and the wrap would be refused).
      /// A rotating slot, like the rsrp ring - a pooled estimator may have later hops in flight.
      float* dst = nullptr;
      /// Transform size of one slice. The HOST derives it the way the estimator's get_idft() does (the
      /// next power of two of (nof_re * 4096 / MAX_NOF_SUBCARRIERS), at least 2048), so both sides
      /// transform the same number of points and resolve the same delay the same way.
      unsigned dft_size = 0;
      /// Pilot spacing in subcarriers: 1 PUCCH f1/3/4 (and the sparse-mask route, whose positions are
      /// the subcarrier offsets themselves), 2 PUSCH, 3 PUCCH f2. It is what turns a tap index into a
      /// time, so a wrong value scales the estimate rather than failing.
      unsigned stride = 0;
      /// Subcarrier spacing of the hop, in Hz.
      float scs_hz = 0.0F;
      /// Half-cyclic-prefix search window, in taps of the profile.
      unsigned max_ta_samples = 0;
      /// DM-RS symbols OF THE HOP, ascending: slice s of the placement is dmrs_slots[s] crossed with
      /// the layers, which is the order the host's estimator enumerates its slices in. Not the slot's
      /// DM-RS symbols (the geometry carries those): a hop of a frequency-hopping slot covers only
      /// some of them, and using the slot's would transform the other hop's pilots as well.
      /// MAX_DMRS_SYMBOLS (4) entries; the kernel's parameter block hard-codes that size.
      static constexpr unsigned max_dmrs_symbols = 4;
      unsigned                  nof_dmrs_symbols = 0;
      unsigned                  dmrs_slots[max_dmrs_symbols] = {};
    };

    /// Left empty (dst == nullptr) when the caller wants the host to keep computing the TA, which is
    /// what OCUDU_CE_DEV_STATS=0 asks for.
    ta_stage_t ta;
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
    /// \brief BASE address of the caller's noise-variance buffer, or nullptr to load A's diagonal
    /// from \c sigma2 (the host's value).
    ///
    /// It is the estimator's sigma2 buffer, the one the extraction's own command buffer filled (see
    /// pilots_stage::sigma2). Pointing at it removes the host from between the received grid and A:
    /// the kernel reads the loading where the device left it, so a hop whose extraction, correlation
    /// and weights all run on the device never has the host read a scalar that its own device work
    /// produced.
    ///
    /// \warning It is the BASE of the buffer, NOT the address of the ratio: the kernel indexes it with
    /// \c sigma2_slot (see there). Handing over the element's own address would make the kernel's
    /// scalars[slot] land past the end of the buffer.
    /// \warning The two select the SAME float, and that is a measured property, not a construction:
    /// the device quotient is bit-identical to \c sigma2 only because ocudu_mmse_pilots.metal is
    /// compiled with -fno-fast-math (under the default fast math it differed in 28.4% of 2^20 pairs
    /// and flipped LLR decisions on 27 of 27 captures - see that file and CMakeLists.txt). Anything
    /// that perturbs the host's ratio (OCUDU_CE_PP_PERTURB) must therefore leave this null.
    /// \warning Only valid while the extraction buffer holds THIS hop's ratio, and only when the
    /// extraction stage actually ran for it (pilots_stage::sigma2_done) - a stale pointer would load A
    /// with the previous hop's noise, which is a silent, small error.
    const float* sigma2_dev = nullptr;
    /// \brief Element of \c sigma2_dev the kernels read, or 0 for "use \c sigma2 instead".
    ///
    /// The estimators that own the buffer know which slot holds what (the estimator's is 2, the
    /// noise-to-pilot-power ratio); the engine must not assume it. 0 is the "no device loading" value
    /// on purpose, which is why the buffer's slot 0 is never used for this.
    unsigned sigma2_slot = 0;
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

  /// \brief The same build, in a command buffer of its own that is committed WITHOUT waiting, plus the
  ///        back-end stage fence so the caller's next submission can order itself after it.
  ///
  /// This is the third form of one build, and it exists for a measured reason. The prefix form (the
  /// build riding the weights command buffer, correlation_stage() handed to run_async()) is the cheap
  /// one - one submission - and it is WRONG on about a quarter of the runs: the memory barrier between
  /// the prefix and K1 does not make the prefix's writes visible to K1, so K1 occasionally inverts a
  /// half-written A and the noise variance explodes (design document 5.9.88). The standalone form
  /// (build_correlation()) is right because it commits AND waits, and the wait is what costs: +39.5us
  /// on a 223us hop. This form keeps the build in its own command buffer and the ordering, but moves
  /// the wait off the host: the buffer signals the shared back-end fence at its completion and the
  /// caller encodes a wait for that generation in its own buffer, so the GPU orders itself and the host
  /// never blocks.
  ///
  /// \note Deliberately NOT built on begin_stage(): that closes (and waits for) an extraction buffer
  ///       held open for the weights stage, and holding it is what makes the hop one submission. This
  ///       opens its own command buffer directly, so a held extraction is left alone.
  /// \return The fence generation to wait for, or 0 when nothing was armed (the caller then falls back
  ///         to the prefix form).
  uint64_t build_correlation_fenced(const corr_stage& c, unsigned nof_systems);

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
    /// \brief The two scalars the kernels derive the symbol start epochs from (batch 5g).
    ///
    /// The CFO kernels of this stage used to read a 14-float array the estimator uploaded into a
    /// device buffer; they now derive the hop's few start times from the numerology and the cyclic
    /// prefix (see ocudu_mmse_epochs.h), so this stage neither wraps nor binds that buffer.
    unsigned numerology  = 0;
    bool     cp_extended = false;
    /// \brief Start-time span of the hop's first two DM-RS symbols, in symbol durations.
    ///
    /// The one epoch-derived number the CFO estimator needs, and the only one it is GIVEN: its answer
    /// comes out of a 72-term accumulation, so the shape the compiler picks for that accumulation
    /// decides the published bits, and computing the epochs inside it moved the estimate by 1 ulp
    /// (measured, batch 5g - see mmse_pilots_cfo). Derived by the estimator from the host array it has
    /// held all along: epoch[dmrs_symb[1]] - epoch[dmrs_symb[0]], the same expression the host's own
    /// estimator divides by.
    float    epoch_span = 0.0F;
    /// Destination of the least-squares pilots, [symbol][layer][pilot] real/imag interleaved.
    float* lse = nullptr;
    /// Destination of the estimated CFO (a single float).
    float* cfo = nullptr;
    /// \brief The PREVIOUS hop's CFO slot, read by the kernel to carry a value forward (a single
    /// float), or nullptr when the caller carries it on the host instead.
    ///
    /// \c mmse_pilots_cfo can only estimate a CFO from TWO DM-RS symbols, so a hop with one of them
    /// has nothing to write - and a consumer of this slot must still find the last value that WAS
    /// estimated. The host used to reproduce that by copying the previous slot into this one before
    /// submitting, which cost a device -> host read and a host -> device write on EVERY hop (the
    /// copy ran whether or not the kernel was going to overwrite it, because it has to happen before
    /// the extraction is submitted).
    ///
    /// Handing the previous slot to the kernel lets IT keep the invariant "every slot holds the most
    /// recently written value" - it writes the carry when it has nothing to estimate - so the host
    /// no longer touches the array at all. It is the same one-line copy, done on the side that
    /// already owns the data.
    const float* cfo_prev = nullptr;
    /// Received DM-RS of the hop, [symbol][cdm][pilot] real/imag interleaved - the same array the
    /// equalizer's noise reduction (K4) reads. The noise variance below needs it.
    ///
    /// It is an INPUT on the routes where the host extracts the pilots, and the extraction kernel's
    /// OUTPUT on the routes where the device does (batch S13-P2: mmse_pilots_lse() stores the rx it
    /// reads into this same buffer, which is what let the host stop extracting - and therefore stop
    /// reading the device's grid and handing the values straight back).
    float* rx_pilots = nullptr;
    /// CAPACITY of \c rx_pilots in bytes.
    std::size_t rx_bytes = 0;
    /// \brief Destination of the hop's EPRE reduction (the SUM of |rx|^2 over \c rx_pilots), ONE
    /// float, or nullptr to skip it.
    ///
    /// EPRE is a REPORTING value the estimator's base class accumulated from the received pilots it
    /// extracted itself. On a hop whose received pilots the DEVICE builds there is no host copy to
    /// accumulate, so the same sum is reduced here - in the extraction's own command buffer, right
    /// after the kernel that produces the values - and the host adds it to the statistic where its
    /// own per-symbol terms used to go (see mmse_pilots_epre, and
    /// port_channel_estimator_average_impl::get_device_epre_sum()).
    float* epre = nullptr;
    /// Destination of the frequency-smoothed pilots, [symbol][layer][pilot] real/imag interleaved.
    /// A scratch buffer of the same size as \c lse: the smoothing is what the noise variance is
    /// estimated from, and the LSE itself must survive (the weights' y vectors are built from it
    /// later, in the engine's own command buffer).
    float* smoothed = nullptr;
    /// Destination of the hop's noise variance, of the pilots' power sum, of their mean and of the
    /// ratio the host computes from them (FOUR floats: [0] sigma2, [1] the sum of |LS pilot|^2,
    /// [2] sigma2 / max([1] / nof_power_pilots, 1e-30F) - the host's own quotient, bit for bit - and
    /// [3] the mean [1] / nof_power_pilots), or nullptr to skip all of them. When set, the extra
    /// dispatches below ride THIS command buffer, so the host reads two scalars after the wait instead
    /// of running estimate_sigma2() (measured 3.3 us per hop of host time on air) and of walking the
    /// pilots for their mean power. The power sum and the ratio are skipped - and left untouched -
    /// whenever this whole block is (see sigma2_done).
    ///
    /// \c [2] is what lets the correlation stage - and, through it, the weights and the published
    /// estimate - be built without the host passing the ratio in: hand this pointer to
    /// corr_stage::sigma2_dev and A's diagonal loading comes from here. It is the host's float only
    /// because ocudu_mmse_pilots.metal is compiled with -fno-fast-math (see there).
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
    /// \brief Number of pilots the mean-power reduction divides its sum by, i.e. the caller's own
    /// nof_power_pilots (nof_layers * nof_dmrs_symbols * nof_symbol_pilots). 0 means "no mean and no
    /// ratio wanted": both slots are then left at the value the caller's own fallback produces (0).
    ///
    /// Only the \c sigma2 buffer's third and fourth slots use it (see there).
    unsigned nof_power_pilots = 0;
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

    /// \brief Whether build_pilots_lse() may leave its command buffer OPEN for the weights stage, so
    ///        that the hop has ONE estimator submission instead of two (batch S13-P2).
    ///
    /// False (the default, and what every caller that reads the extraction's results needs):
    /// build_pilots_lse() ends, commits and WAITS its command buffer before returning, so the host may
    /// read the least-squares pilots, the CFO, the noise-variance block and the EPRE sum right away.
    ///
    /// True: the caller PROMISES that nothing on the host reads any of those before it encodes the
    /// weights stage (run()/run_async()/run_weights_only*()). The buffer is then held in the engine and
    /// adopted by that call, which opens a SECOND encoder on it and commits once for both stages. Two
    /// encoders of one command buffer ARE ordered - measured, not assumed: case F of
    /// doc_chinese/phy_pipeline_gpu/wip/metal_alias_order.mm passes 200/200 with the same MTLBuffer
    /// object and no barrier between the encoders, while an ALIASED pair fails 200/200 even across
    /// encoders (case G). That is why this promise only holds together with wrap()'s
    /// one-object-per-region rule.
    ///
    /// A held buffer that the weights never adopt is committed and waited by wait_pending(), so an
    /// abandoned hop still publishes its extraction rather than losing it. Not honoured in \c burst
    /// lane order, where the weights join the lane's shared command buffer instead (there is nothing
    /// for the extraction's own buffer to be adopted by).
    bool hold_for_weights = false;
  };

  /// \brief Runs the estimator's input stage (K0-a) on the device: pilot extraction from the
  /// device-resident grid, least-squares estimates, CFO estimation and compensation.
  ///
  /// The host consumes \c lse right afterwards (it is the estimator's input), so this completes
  /// synchronously - unless the caller promises otherwise (pilots_stage::hold_for_weights), in which
  /// case the buffer is held open for the weights stage and the two share one submission.
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

  /// Whether the metallib carries the device-side rsrp reduction (mmse_rsrp). When false the caller
  /// must keep reducing the pilots on the host - the same shape as scatter_available().
  bool rsrp_available() const;

  /// \brief Whether the metallib carries the received pilots' EPRE reduction (mmse_pilots_epre).
  ///
  /// S13-P2: the estimator stops extracting the received pilots on the hops where the device builds
  /// them, and the EPRE statistic is then produced here. A metallib that builds the pilots but cannot
  /// reduce them leaves the caller with no statistic for such a hop, so it must keep the host
  /// extraction - which is what port_channel_estimator_metal_mmse_impl::stage_produces_hop_inputs()
  /// asks before answering.
  bool epre_available() const;

  /// Whether the metallib carries the device-side time-alignment reduction (mmse_ta_profile).
  bool ta_available() const;

  /// \brief The hop's time alignment, reduced on the device out of the per-symbol IDFTs.
  ///
  /// The transform itself is the DFT engine's (see ocudu_dft.metal); this reduces its output the way
  /// time_alignment_estimator_dft_impl::estimate_ta_correlation() does on the host - accumulate
  /// |.|^2 over the slices, search half a cyclic prefix either side of the circular profile, refine
  /// the peak with a parabolic fit - and returns SECONDS, the unit the report is in.
  ///
  /// \param[in]  slices  \c nof_slices consecutive transforms of \c size complex values (float
  ///                     pairs), exactly as the DFT engine lays a batch out.
  /// \param[in]  size    Transform size of one slice.
  /// \param[in]  nof_slices DM-RS symbols times layers of the hop.
  /// \param[in]  stride  Pilot spacing in subcarriers (1 PUCCH f1/3/4, 2 PUSCH, 3 PUCCH f2): it is
  ///                     what turns the tap index into a time, so it must match the geometry.
  /// \param[in]  scs_hz  Subcarrier spacing of the hop, in Hz.
  /// \param[in]  max_ta_samples Half-cyclic-prefix search window, in taps.
  /// \param[out] ta_seconds The estimate. Untouched when the call fails.
  /// \return True on success. False when the kernel is unavailable, the geometry is outside its
  ///         contract, or the dispatch failed - the caller then keeps the host's estimate.
  bool run_ta_profile(const void* slices,
                      unsigned    size,
                      unsigned    nof_slices,
                      unsigned    stride,
                      double      scs_hz,
                      unsigned    max_ta_samples,
                      float&      ta_seconds);

  /// \brief The geometry of the hop's estimates, as the reformat's own parameters describe them.
  ///
  /// K5 (rsrp) and K7 (the time-alignment placement) read the VERY SAME buffer with the same
  /// indexing - the reformat's source \c h, [nof_systems][n_blk + tail slots][2 * nout_stride] - so
  /// they are handed the same struct rather than two copies of eleven fields that can drift apart.
  /// Field for field the leading part of mmse_rsrp_params and mmse_ta_place_params in the metallib.
  struct hop_geometry {
    /// Output positions per block in the batch (the row length of h, in complex values).
    unsigned nout_stride = 0;
    /// STANDARD blocks per system. The batch's edge block, when it carries one, is the block slots
    /// from n_blk on: it is block 0 of the systems from sys_tail on, not an out-of-range block.
    unsigned n_blk = 0;
    /// Subcarriers per standard block.
    unsigned nf_std = 0;
    /// First subcarrier of the edge block (n_blk * nf_std; the hop's span when there is none).
    unsigned sc_tail_base = 0;
    /// Subcarriers of the edge block (0 when the hop has none).
    unsigned nf_tail = 0;
    /// First system of the edge block (== nof_layers).
    unsigned sys_tail = 0;
    unsigned nof_layers = 0;
    unsigned nof_symbols = 0;
    /// DC subcarrier of the hop (>= the hop's span when there is none). K5 leaves it out of its sum;
    /// K7 does not (the host's TA input includes it) - see ocudu_mmse_ta.metal.
    unsigned dc_sc = 0;
    /// DM-RS symbols of the slot, one bit per symbol.
    unsigned dmrs_sym_bits = 0;
    /// The layer's own pilot comb within a PRB, one 12-bit mask per layer (MAX_LAYERS entries).
    static constexpr unsigned        max_layers = 4;
    std::array<unsigned, max_layers> pilot_re_bits{};

    /// \brief The geometry as the kernels' parameter block reads it.
    ///
    /// The eleven fields above followed by the four comb masks, in the order BOTH kernels declare them
    /// (mmse_rsrp_params and mmse_ta_place_params, ocudu_mmse_rsrp.metal / ocudu_mmse_ta.metal). One
    /// place builds it, so K5 and K7 cannot be handed geometries that disagree about which subcarrier
    /// is which - they read the very same buffer.
    std::array<uint32_t, 14> words() const
    {
      std::array<uint32_t, 14> w{static_cast<uint32_t>(nout_stride),
                                 static_cast<uint32_t>(n_blk),
                                 static_cast<uint32_t>(nf_std),
                                 static_cast<uint32_t>(sc_tail_base),
                                 static_cast<uint32_t>(nf_tail),
                                 static_cast<uint32_t>(sys_tail),
                                 static_cast<uint32_t>(nof_layers),
                                 static_cast<uint32_t>(nof_symbols),
                                 static_cast<uint32_t>(dc_sc),
                                 static_cast<uint32_t>(dmrs_sym_bits)};
      for (unsigned i = 0; i != max_layers; ++i) {
        w[10 + i] = static_cast<uint32_t>(pilot_re_bits[i]);
      }
      return w;
    }
  };

  /// \brief Places a hop's pilots into a DFT input for the time-alignment port (K7).
  ///
  /// The transform that follows is the DFT engine's kernel (dft_dit), which reads its input as
  /// `in[perm[i]]` - a gather with no subcarrier stride. This scatters the pilots of every DM-RS
  /// symbol and layer into its own slice of the transform input, at the positions the host's own
  /// estimator uses, and zeroes the rest of the slice: the hop's alignment can then be reduced on the
  /// device without the host assembling (and therefore committing and waiting for) the input.
  ///
  /// \param[in]  h          the reformat's SOURCE, [nof_systems][n_blk slots][2 * nout_stride] floats,
  ///                        the buffer K5 also reads.
  /// \param[in]  geometry   the hop's geometry (see hop_geometry).
  /// \param[in]  dft_size   Transform size of one slice; the size the twiddle/permutation tables are
  ///                        built for.
  /// \param[in]  stride     Pilot spacing in subcarriers, as the host's estimator is called with.
  /// \param[out] dst        [nof_slices][dft_size] complex pairs, \c nof_slices = nof_symbols x
  ///                        nof_layers. Every position of every slice is written.
  /// \return True when the placement was encoded and completed.
  bool run_ta_place(const void*         h,
                    const hop_geometry& geometry,
                    unsigned            dft_size,
                    unsigned            stride,
                    void*               dst);

  /// Whether the metallib carries the placement kernel, and the DFT kernel it feeds, for \c dft_size.
  bool ta_place_available(unsigned dft_size);

  /// \brief Batch 5g's self-check: the slot's symbol start epochs, computed by the SHADER.
  ///
  /// The kernels derive the epochs from (numerology, CP) instead of reading a 14-float array the host
  /// uploaded once per configuration - the lane's last host -> device write - so the array they replace
  /// is the reference, and this returns what the KERNEL computes for one (numerology, CP) pair to
  /// compare against it. The unit test sweeps both CP types and all five numerologies, which is the
  /// whole domain; the lane never calls it.
  ///
  /// \param[in]  numerology  Numerology index mu (clamped by the kernel to the SCS enum's range).
  /// \param[in]  cp_extended Non-zero for extended cyclic prefix.
  /// \param[out] dst         MAX_NSYMB_PER_SLOT (14) floats, every one written.
  /// \return True when the metallib carries the probe and the dispatch completed.
  bool run_epoch_probe(unsigned numerology, unsigned cp_extended, float* dst);

  /// \brief Whether the metallib carries the FUSED time-alignment chain (batch 5d).
  ///
  /// That is the kernel the lane runs: placement, transform, power delay profile and peak in ONE
  /// dispatch. Bounded by the profile it keeps in threadgroup memory, so \c dft_size must be at most
  /// 2048 - the largest size the estimator's get_idft() can ask for.
  bool ta_chain_available(unsigned dft_size) const;

  /// \brief The hop's time alignment in ONE dispatch, out of the reformat's own h.
  ///
  /// The same chain run_ta_place() + the DFT engine + run_ta_profile() implement in three, and the one
  /// the lane encodes (see encode_ta()): the placement and the transform happen inside the same
  /// threadgroup, so nothing crosses a dispatch boundary and no intermediate buffer exists.
  ///
  /// \param[in]  h          the reformat's SOURCE, the buffer K5 also reads.
  /// \param[in]  geometry   the hop's geometry (see hop_geometry).
  /// \param[in]  dft_size   Transform size of one slice (<= 2048; the twiddle table is built for it).
  /// \param[in]  stride     Pilot spacing in subcarriers, as the host's estimator is called with.
  /// \param[in]  nof_dmrs_symbols The HOP's DM-RS symbol count (the slice count is this times layers).
  /// \param[in]  dmrs_slots The hop's DM-RS slot symbols, ascending.
  /// \param[in]  scs_hz     Subcarrier spacing of the hop, in Hz.
  /// \param[in]  max_ta_samples Half-cyclic-prefix search window, in taps.
  /// \param[out] ta_seconds The estimate, in SECONDS.
  /// \return True when the dispatch was encoded and completed.
  bool run_ta_chain(const void*         h,
                    const hop_geometry& geometry,
                    unsigned            dft_size,
                    unsigned            stride,
                    unsigned            nof_dmrs_symbols,
                    const unsigned*     dmrs_slots,
                    double              scs_hz,
                    unsigned            max_ta_samples,
                    void*               ta_seconds);

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
                 unsigned              nof_scatter = 0,
                 const corr_stage*     corr_edge   = nullptr);

  /// \brief Selects where the following stages put their dispatches and how the lane is ordered after
  /// them (see ce_lane_order).
  ///
  /// The order is per hop, and the adapter sets it on EVERY hop both ways: the adapter outlives the hop,
  /// and an engine left in \c burst mode would silently dispatch a later synchronous hop into a burst
  /// nobody owns.
  ///
  /// The default of a fresh engine is \c host_wait - the behaviour every caller of this engine had before
  /// the lane orders existed, which is what a caller that does not know about them must keep. The
  /// receiving chain passes \c event (see port_channel_estimator_metal_mmse_impl::ce_lane_order_from_env()).
  ///
  /// \param[in] order The order the following deferred stages use.
  void set_lane_order(ce_lane_order order);

  /// The order this engine's stages currently use.
  ce_lane_order lane_order() const;

  /// \brief Tells the engine which resource grid the next hop reads, so it can ADOPT the DFT's block (D1).
  ///
  /// The receiving chain opens one command buffer per slot for that slot's transforms and hands it over
  /// UNCOMMITTED, keyed by the resource grid it wrote (see dft_metal_engine::release_block() and
  /// shared_burst::deposit_released()). The estimator's extraction - the hop's first back-end stage - is
  /// where that buffer is claimed: with this key set, build_pilots_lse() adopts it instead of opening a
  /// command buffer of its own, and the hop becomes ONE submission whose first dispatches are the
  /// transforms that produced the grid it is about to read.
  ///
  /// Not a hint and not a knob: the ADAPTER passes the grid of the hop it is about to run, once per hop
  /// (the engine does not keep it), and a hop whose grid has nothing deposited behaves exactly as before.
  ///
  /// \param[in] grid_base Storage base of the grid the next hop reads
  ///            (resource_grid_reader::get_device_view().base), or nullptr for a hop that must not adopt.
  /// \param[in] slot      The receiving slot of that grid. The storage ADDRESS is reused by the grid pool
  ///            from one slot to the next, so it does not identify the block by itself: a hop that names the
  ///            wrong slot would adopt the wrong block and read a grid nobody wrote (5.9.15).
  void set_hop_grid(const void* grid_base, uint64_t slot);

  /// \brief Waits for the submission of run_async() and reports whether it completed.
  /// \return True when there was nothing pending, or when the pending submission succeeded.
  bool wait_pending();

  /// \brief Completes the dispatches a \c burst-order stage left in the shared burst, and reports success.
  ///
  /// In \c burst order (set_lane_order()) the estimator leaves its dispatches in the command buffer the
  /// rest of the receiving chain shares, so that the lane's own commit and wait cover them: by the time the
  /// estimator is asked to complete the hop, this call normally finds nothing outstanding and returns at
  /// once. The callers that read the hop's results BEFORE the lane commits are the ones that need it to do
  /// the work - the demodulator on the route that syncs the estimates to host memory (OCUDU_CE_CPU_CE),
  /// which completes the estimation before it submits the equalization, and the debug capture. The burst
  /// then holds this engine's dispatches alone (no other stage has encoded into it yet), so committing it
  /// here gives up the overlap, never the ordering the burst exists for.
  ///
  /// \note In \c event and \c host_wait order the hop's dispatches are in the ENGINE's own command buffer,
  ///       and the counterpart of this call is wait_pending(). The caller knows which one it left behind
  ///       (see port_channel_estimator_metal_mmse_impl::pending_fused_burst).
  /// \return True when everything the calling thread's lane holds completed successfully.
  bool complete_fused_burst();

  /// \brief Self-test of the back-end stage fence (S-7g-19): encodes the very wait the lane burst encodes,
  /// on a command buffer of its own, and waits for it.
  ///
  /// This is what lets the unit test judge the ordering mechanism instead of only its result: a lane that
  /// waits for a generation nobody signalled would otherwise be invisible until it read stale weights on
  /// air. It opens a back-end command buffer, calls the same shared_queue::backend_stage_wait() that
  /// shared_burst uses, commits it and waits for it - so a wait that names a generation no command buffer
  /// will ever signal shows up here as a command buffer that never completes.
  ///
  /// \param[out] waited True when a wait was encoded (i.e. an estimator commit had been signalled).
  /// \return True when the command buffer completed successfully.
  static bool lane_fence_selftest(bool& waited);

  /// Diagnostics of the back-end stage fence, for the unit test and the [metal_stats] line.
  static uint64_t lane_fence_generation();
  static uint64_t lane_fence_nof_signals();
  static uint64_t lane_fence_nof_waits();
  static uint64_t lane_fence_nof_skipped_waits();

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
