// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief K0-a: the channel estimator's INPUT stage, built on the device.
///
/// The estimator's input is the set of least-squares channel estimates at the DM-RS pilots -
/// `pilots_lse_view` today. The host builds it in
/// port_channel_estimator_average_impl::compute_hop_submit(), whose own comment names the four
/// steps: "pilot extraction from the resource grid, EPRE, LSE and CFO". This file takes the first
/// and the last two over:
///
///   1. extraction: the DM-RS resource elements of each DM-RS symbol, read from the frequency-domain
///      grid. The grid is ALREADY device-resident (S-7b: the FFT writes into it and the host shares
///      the same memory), so nothing has to be brought over - the kernel reads it in place through
///      the descriptor in resource_grid_device_view.h.
///   2. LSE: multiply by the conjugate of the transmitted DM-RS (ocuduvec::prod_conj).
///   3. CFO: estimate it from the phase between the first two DM-RS symbols, then compensate both.
///
/// ---- Numerics: this kernel does NOT have to be bit-exact with the host ----
/// Unlike K0-d (ocudu_mmse_corr.metal), whose 1-ulp deviation was amplified by cond_2(A) ~ 2e4 into
/// ~1% of W and h, the pilots enter the output LINEARLY: h = W . y, so a relative error in y passes
/// to h unchanged, with no amplification factor (W is well conditioned). One ulp here is therefore
/// irrelevant, which is why this file needs neither exact expression ordering nor
/// IEEE_MATH_SOURCES. The acceptance is a tolerance comparison on the pilots plus the end-to-end
/// gates. (The general rule: estimate the amplification from the quantity to the output - near 1
/// means tolerance, a condition-number-sized factor means bit-exact reproduction.)
///
/// What IS reproduced exactly is the part that costs nothing to reproduce:
///   - the pilot mapping (PRB-major, re_pattern ascending), the same convention
///     ocudu_mmse_corr.metal already uses;
///   - cbf16 -> float, which is a pure bit operation (as_type<float>(u << 16), see adt/bf16.h), so
///     the extraction introduces no rounding of its own;
///   - the host's accumulation ORDER is followed where it is visible (see the CFO reduction).

/// ---- STATUS (S-7f-5h): dispatched by default, pilot-level probe green ----
/// These kernels produce the estimator's input: port_channel_estimator_metal_mmse_impl::apply_fd_
/// td_estimation_stage() builds a pilots_stage from the grid's device view and overwrites
/// pilots_lse_view with the result. OCUDU_CE_CPU_LS=1 forces the host pre-stage instead, and the
/// device path falls back to it automatically when the grid has no device view, the allocation is
/// not contiguous, or the pilot count disagrees with the caller's.
///
/// Measured: the pilots match the host to 1.4-2.2e-07 maximum relative difference with none outside
/// 1e-5 on the three reference captures, and the end-to-end result is unchanged. Gates with this as
/// the default: k0d 980/980 byte-identical, k1 980/980 decision-identical, combos PASS,
/// ctest -L phy 162/162.
///
/// Resolved since S-7f-5c, both worth remembering:
///   - the host compensates the CFO through TWO call sites (compensate_cfo_and_accumulate() for
///     symbols 0 and 1, combine_pilots() for the rest), each with the SLOT SYMBOL epoch of its own
///     DM-RS symbol. An earlier version stopped at the first two, which left a third of a
///     three-symbol hop rotated by ~6% while symbols 0 and 1 matched bit for bit;
///   - the time-domain strategy is not a variable here: the metal estimator FORCES
///     td_interpolation_strategy::interpolate in its constructor ("so the LSE pilots are kept per
///     DM-RS symbol"), so the per-symbol products this file produces are the right shape.
///
/// LAYER COUNT (S-7f-5k): the pilot positions come from dmrs_patterns.front().re_pattern for every
/// layer, and that is EXACT FOR ANY NUMBER OF LAYERS - not a gap. An earlier revision of this comment
/// claimed 4-layer allocations would need a per-layer pattern; checking the producer showed
/// otherwise: dmrs_pusch_estimator_impl assigns the same pattern to every layer
/// (`mask[i_layer].re_pattern = params.re_pattern`, dmrs_pusch_estimator_impl.cpp:172, inside
/// `for (i_layer = 0; i_layer != nof_tx_layers; ++i_layer)`) and separates the layers with the
/// orthogonal cover codes w_t (per symbol) and w_f (per subcarrier) applied to each layer's own
/// REFERENCE sequence - which is CDM, and which this file already stages per layer. All layers
/// therefore sit on the same REs.
///
/// Still the host's, deliberately (the CPU glue this step keeps): EPRE, sigma2 and the FD smoothing
/// of filtered_pilots_lse, plus the fact that the base class still runs its own pre-stage which
/// these kernels overwrite.
///
/// ---- STATUS (S-7f-5u): glue #2 - the device also writes the engine's pilot vectors ----
/// The least-squares pilots above are the estimator's INPUT. The engine's weights do not read them
/// where this file leaves them: they read a re-indexed block layout called y, which the host used
/// to build by copying gpu_ls_out back to pilots_lse_view and then copying that into the y slots
/// (two host memcpys per group per hop). mmse_pilots_scatter_y() below does the re-indexing on the
/// device, inside the weights' own command buffer, so neither copy happens.
///
/// It is a re-index and a multiply ONLY, and the multiply is the same single-precision product the
/// host applies (ocuduvec::sc_prod(pilots, inv_beta)): this file therefore still needs neither exact
/// expression ordering nor IEEE_MATH_SOURCES, and "device-written y" is byte-identical to
/// "host-staged y" - which is what makes OCUDU_CE_DEV_Y=0 an exact A/B (capture_gates.sh ydev).

#include <metal_stdlib>
using namespace metal;

struct mmse_pilots_params {
    uint nof_dmrs_symb;     // DM-RS symbols of the hop (the second grid dimension is symb x layer)
    uint nof_layers;        // Tx layers
    uint nof_pilots;        // pilots per DM-RS symbol = nof_prb * ncomb
    uint ncomb;             // pilots per PRB of a DM-RS symbol (the DM-RS comb size)
    uint nof_prb;           // PRBs of the hop
    uint first_prb;         // first PRB of the hop within the grid
    uint port;              // receive port the grid view belongs to
    uint grid_subc_stride;  // elements between two consecutive subcarriers of the grid view
    uint grid_symb_stride;  // elements between two consecutive OFDM symbols of the grid view
    uint grid_port_stride;  // elements between two consecutive ports of the grid view
    // Slot symbol index of each hop DM-RS symbol, in hop order.
    uint dmrs_symb[4];
    // Pilot positions within a PRB, ascending (ncomb entries).
    uint pilot_re[12];
};

/// Subcarrier of the \c i_pilot -th pilot of the hop: PRB-major, then re_pattern ascending - the
/// same mapping as extract_re_prb() and as mmse_corr_pilot_subcarrier() in ocudu_mmse_corr.metal.
static inline uint mmse_pilots_subcarrier(constant mmse_pilots_params& p, uint i_pilot)
{
    return (p.first_prb + i_pilot / p.ncomb) * 12u + p.pilot_re[i_pilot % p.ncomb];
}

/// bfloat16 -> float, bit for bit: the value is the top half of the float, so shifting it up and
/// reinterpreting it is exact (include/ocudu/adt/bf16.h: to_float()).
static inline float mmse_bf16_to_float(ushort u)
{
    return as_type<float>(static_cast<uint>(u) << 16);
}

/// \brief Reads one DM-RS resource element of the grid view: the received pilot, as a float2.
static inline float2 mmse_pilots_read_grid(device const ushort*          grid,
                                           constant mmse_pilots_params& p,
                                           uint                         i_symb,
                                           uint                         i_pilot)
{
    const ulong re = static_cast<ulong>(p.port) * p.grid_port_stride +
                     static_cast<ulong>(p.dmrs_symb[i_symb]) * p.grid_symb_stride +
                     static_cast<ulong>(mmse_pilots_subcarrier(p, i_pilot)) * p.grid_subc_stride;
    return float2(mmse_bf16_to_float(grid[2 * re]), mmse_bf16_to_float(grid[2 * re + 1]));
}

/// \brief Fills the LSE pilots of the whole hop: lse[symb][layer][pilot] = rx . conj(ref).
///
/// One thread per (pilot, symbol x layer). This is steps (1) and (2) above; the CFO is applied by
/// mmse_pilots_apply_cfo() after mmse_pilots_cfo() has estimated it, exactly as the host splits
/// preprocess_pilots_and_estimate_cfo() from compensate_cfo_and_accumulate().
kernel void mmse_pilots_lse(device const ushort*         grid    [[buffer(0)]],
                            device const float*          ref     [[buffer(1)]], // [symb][layer][pilot], cf32
                            device float*                lse     [[buffer(2)]], // [symb][layer][pilot], cf32
                            constant mmse_pilots_params& p       [[buffer(3)]],
                            uint2                        gid     [[thread_position_in_grid]])
{
    if ((gid.x >= p.nof_pilots) || (gid.y >= p.nof_dmrs_symb * p.nof_layers)) {
        return;
    }
    const uint i_symb  = gid.y / p.nof_layers;
    const uint i_layer = gid.y % p.nof_layers;
    const uint i_pilot = gid.x;

    const float2 rx = mmse_pilots_read_grid(grid, p, i_symb, i_pilot);

    const ulong  ref_i = (static_cast<ulong>(gid.y) * p.nof_pilots + i_pilot) * 2;
    const float2 r     = float2(ref[ref_i], ref[ref_i + 1]);

    // ocuduvec::prod_conj(): rx . conj(r).
    lse[ref_i]     = rx.x * r.x + rx.y * r.y;
    lse[ref_i + 1] = rx.y * r.x - rx.x * r.y;
}

/// \brief Estimates the CFO from the phase between the first two DM-RS symbols.
///
/// Reproduces preprocess_pilots_and_estimate_cfo() exactly, including its STRUCTURE: the host calls
/// it once per CDM group (a pair of consecutive layers), and each call
///     acc = SUM over the group's layers of dot_prod(lse[1], lse[0])
///         = SUM_l SUM_i lse[1][l][i] . conj(lse[0][l][i])
///     cfo_group = arg(acc) / 2pi / (epoch[dmrs_1] - epoch[dmrs_0])
/// returns one CFO per group, and the caller AVERAGES those group CFOs
/// (preprocess...: returns `cfo`; compute_hop_submit(): `cfo_hop = transform_optional(cfo_hop,
/// divides, divide_ceil(nof_tx_layers, 2))`). Taking arg() once over a sum of all layers would be a
/// different number, so the groups are kept separate here.
///
/// One threadgroup, thread 0: the reduction is a few hundred complex MACs and a serial walk keeps
/// the accumulation order closest to the host's. \c out[0] carries the CFO.
kernel void mmse_pilots_cfo(device const float*          lse    [[buffer(0)]],
                            device const float*          epochs [[buffer(1)]], // symbol start times
                            device float*                out    [[buffer(2)]], // [1]: the CFO
                            constant mmse_pilots_params& p      [[buffer(3)]],
                            uint                         tid    [[thread_position_in_threadgroup]])
{
    if ((p.nof_dmrs_symb < 2) || (tid != 0)) {
        return;
    }
    const uint  nof_groups = (p.nof_layers + 1u) / 2u;
    const ulong sym0_base  = 0;
    const ulong sym1_base  = static_cast<ulong>(p.nof_layers) * p.nof_pilots * 2;

    float cfo_sum = 0.0F;
    for (uint g = 0; g != nof_groups; ++g) {
        float2 acc = float2(0.0F, 0.0F);
        for (uint l = 2 * g; (l != 2 * g + 2) && (l != p.nof_layers); ++l) {
            const ulong lb = static_cast<ulong>(l) * p.nof_pilots * 2;
            for (uint i = 0; i != p.nof_pilots; ++i) {
                const float2 a = float2(lse[sym1_base + lb + 2 * i], lse[sym1_base + lb + 2 * i + 1]);
                const float2 b = float2(lse[sym0_base + lb + 2 * i], lse[sym0_base + lb + 2 * i + 1]);
                // dot_prod(a, b) = SUM a . conj(b) (ocuduvec/dot_prod.h).
                acc += float2(a.x * b.x + a.y * b.y, a.y * b.x - a.x * b.y);
            }
        }
        const float phase = atan2(acc.y, acc.x);
        const float dt    = epochs[p.dmrs_symb[1]] - epochs[p.dmrs_symb[0]];
        cfo_sum += (dt != 0.0F) ? (phase / 6.283185307179586F / dt) : 0.0F;
    }
    out[0] = cfo_sum / static_cast<float>(nof_groups);
}

/// \brief Compensates the CFO on EVERY DM-RS symbol's LSE pilots, each at ITS OWN epoch.
///
/// The host reaches the same result through two call sites, which is why this is easy to get wrong:
///   - compensate_cfo_and_accumulate() compensates symbols 0 and 1 with
///     polar(1, -2pi . epoch[dmrs_0] . cfo) and polar(1, -2pi . epoch[dmrs_1] . cfo);
///   - combine_pilots() then does symbols 2.. with polar(1, -2pi . epoch[i_symbol] . cfo), i.e. the
///     phasor of the SLOT SYMBOL that symbol occupies.
/// Compensating only the first two (which an earlier version of this kernel did) leaves a third of a
/// three-symbol hop rotated: measured on a capture, symbols 0 and 1 matched the host to the last bit
/// while symbol 2 was off by ~6% of its magnitude.
kernel void mmse_pilots_apply_cfo(device float*                lse    [[buffer(0)]],
                                  device const float*          cfo    [[buffer(1)]], // [1]
                                  device const float*          epochs [[buffer(2)]],
                                  constant mmse_pilots_params& p      [[buffer(3)]],
                                  uint2                        gid    [[thread_position_in_grid]])
{
    if ((gid.y >= p.nof_dmrs_symb) || (gid.x >= p.nof_layers * p.nof_pilots)) {
        return;
    }
    const float  theta = -6.283185307179586F * epochs[p.dmrs_symb[gid.y]] * cfo[0];
    const float2 ph    = float2(cos(theta), sin(theta));

    const ulong base = (static_cast<ulong>(gid.y) * p.nof_layers * p.nof_pilots) * 2 + static_cast<ulong>(gid.x) * 2;
    const float2 v   = float2(lse[base], lse[base + 1]);
    lse[base]     = v.x * ph.x - v.y * ph.y;
    lse[base + 1] = v.x * ph.y + v.y * ph.x;
}

/// ---- Glue #2 (S-7f-5u): the device writes the engine's pilot vectors ---------------------------
///
/// The engine's weights are applied to a BLOCK layout (y) that groups the hop's pilots by block,
/// while this file produces the HOP layout ([symbol][layer][pilot]). The host used to convert
/// between them - twice over: the device result was copied back into pilots_lse_view, and
/// stage_engine_group() then copied pilots_lse_view into the y slots. This kernel does the same
/// conversion where both ends already live: it reads the device's own output and writes the slots
/// the weights kernel reads.
///
/// The host's staging (stage_engine_group(), legacy branch) is the specification:
///     yp = gpu_y + (sys_offset + i_layer) * n_blk * 2 * Ls + b * 2 * Ls
///     yp[2 * (i_symbol * npf + j)] = pilots_lse_view[i_symbol][i_layer][(gb_start + b * b_prb) * comb + j]
///                                     * inv_beta                     (the DM-RS to data scaling)
/// and every field below is one of its terms. The pilot index is expanded rather than kept as a
/// subspan, which is the only re-indexing: (gb_start + b * b_prb) * comb + j == pilot_base + b * npf + j.
///
/// Three regions the host writes and this kernel must reproduce EXACTLY, because the apply kernel
/// reads whatever is there (a non-finite leftover reaches h through 0 * inf = NaN):
///   - the rows past the group's own pilot count (row >= nof_symb * npf) - the pad of a merged
///     batch's narrower tail group, which the host memsets per block;
///   - the block slots a merged tail group does not fill (b >= n_blk_real) - the host memsets the
///     tail group's whole y region before staging it;
///   - everything else is the pilot value times inv_beta.
///
/// One thread per (row, block, layer): the row is the contiguous axis so consecutive threads write
/// consecutive pairs.
struct mmse_scatter_params {
    uint  nof_layers;   // Tx layers = systems of this group
    uint  nof_symb;     // DM-RS symbols of the hop (the first dimension of the LSE layout)
    uint  nof_pilots;   // pilots per (symbol, layer) of the WHOLE hop
    uint  npf;          // pilots one block carries per DM-RS symbol of THIS group = b_prb * ncomb
    uint  pilot_base;   // first pilot of this group inside the hop = gb_start * ncomb
    uint  n_blk_slots;  // block slots per system (the engine's stride: a merged tail group carries
                        // the standard block count while filling only n_blk_real of them)
    uint  n_blk_real;   // blocks this group really fills; the remaining slots are zeroed
    uint  Ls;           // rows of one block slot (the engine's L: n_blk_real * nof_symb * npf with
                        // the merged tail's rows padded up to the standard geometry)
    float inv_beta;     // 1 / beta_scaling, the DM-RS to data scaling
};

kernel void mmse_pilots_scatter_y(device const float*           lse [[buffer(0)]],
                                  device float*                 y   [[buffer(1)]],
                                  constant mmse_scatter_params& p   [[buffer(2)]],
                                  uint3                         gid [[thread_position_in_grid]])
{
    const uint i_layer = gid.z;
    const uint b       = gid.y;
    const uint k       = gid.x; // row of the block slot: i_symb * npf + j
    if ((i_layer >= p.nof_layers) || (b >= p.n_blk_slots) || (k >= p.Ls)) {
        return;
    }
    device float* dst = y + (static_cast<ulong>(i_layer) * p.n_blk_slots + b) * 2 * p.Ls;
    if ((b >= p.n_blk_real) || (k >= p.nof_symb * p.npf)) {
        dst[2 * k]     = 0.0F;
        dst[2 * k + 1] = 0.0F;
        return;
    }
    const uint  i_symb = k / p.npf;
    const uint  j      = k - i_symb * p.npf;
    const ulong src =
        (static_cast<ulong>(i_symb) * p.nof_layers + i_layer) * p.nof_pilots + p.pilot_base + b * p.npf + j;
    dst[2 * k]     = lse[2 * src] * p.inv_beta;
    dst[2 * k + 1] = lse[2 * src + 1] * p.inv_beta;
}

/// ---- S-7f-5w: the hop's noise variance, computed where its inputs already are -------------------
///
/// `estimate_sigma2()` on the host is the last CPU computation on the estimator's IQ-to-LLR path: it
/// smooths the least-squares pilots in frequency (virtual edge pilots + a raised-cosine filter) and
/// then runs the classical noise estimator on the smoothed pilots. Both halves are here, riding the
/// SAME command buffer the pilot extraction already commits and waits for, so the host reads one
/// float instead of computing it (measured on air: 3.3 us per hop).
///
/// ---- TERMINATION AND BOUNDS ARE PARAMETER-INDEPENDENT (hard rule, see the plan's 48.131) --------
/// A GPU kernel that does not finish takes the WEDGE with it: the command buffer never completes, the
/// window server blocks on the GPU and the machine freezes, and only a power cycle recovers it
/// (measured). A wrong parameter must therefore be able to produce a WRONG NUMBER and nothing else:
///   - every loop bound is a compile-time constant (the same maxima the buffers are sized with); the
///     parameters only ever SKIP or BREAK inside those bounds;
///   - every index is clamped into the buffers' maxima before it is used, for reads and for writes;
///   - the threadgroup size is a compile-time constant and is never read back from
///     [[threads_per_threadgroup]], whose scalar form was never established to be safe;
///   - no thread returns before a threadgroup_barrier.
///
/// ---- Numerics: tolerance, not bit-exactness (see the file header and the plan's rule) -----------
/// sigma2 reaches the output only through A's diagonal, where it carries a weight of ~1e-3 (the
/// noise-to-pilot-power ratio) against unit-magnitude off-diagonal entries - an amplification of
/// about one, not the cond_2(A) ~ 2e4 of the correlation kernels. On top of that, the device cannot
/// reproduce libm's cos/sin/atan2 or the host's floating-point contraction exactly, which is the same
/// reason the K0-a pilots themselves are accepted on a tolerance. The gate is therefore the
/// OCUDU_CE_SIGMA2_CHECK probe plus the decision-level A/B (capture_gates.sh sig2), while the
/// byte-identical gates (k0d/k0dm) keep working unchanged: both of their routes read the SAME sigma2.

/// Threadgroup sizes of the two kernels below. Compile-time constants on purpose: they stride the
/// walks, so a zero here would be an infinite loop.
constant uint mmse_smooth_tg_size = 128;
constant uint mmse_sigma2_tg_size = 256;

/// Upper bounds, matching the host's buffers: MAX_V_PILOTS, MAX_FILTER_LENGTH (31), the DM-RS symbol
/// and layer counts, MAX_NSYMB_PER_SLOT and the per-(symbol, layer) capacity of the LSE buffers
/// (MAX_NOF_PILOTS_SYMBOL = MAX_NOF_SUBCARRIERS + 2 * MAX_V_PILOTS = 3300 + 24).
constant uint mmse_max_v_pilots    = 12;
constant uint mmse_max_filter_len  = 31;
constant uint mmse_max_dmrs_symb   = 4;
constant uint mmse_max_layers      = 4;
constant uint mmse_max_cdm         = 2;
constant uint mmse_max_slot_symb   = 14;
constant uint mmse_max_pilots_symb = 3324;

struct mmse_sigma2_params {
    uint  nof_dmrs_symb; // DM-RS symbols of the hop
    uint  nof_layers;    // Tx layers
    uint  nof_pilots;    // pilots per (symbol, layer)
    uint  nof_v_pilots;  // virtual pilots per edge (at most 12)
    uint  filter_len;    // raised-cosine filter length (odd, at most 31)
    uint  nof_cdm;       // CDM groups (the received pilots are indexed by group)
    uint  compensate_cfo;
    float beta;          // DM-RS to data amplitude scaling
    float inv_beta;      // 1 / beta: the caller scales the pilots by it BEFORE smoothing (see below)
    // Slot symbol index of each hop DM-RS symbol (the CFO phasor of estimate_noise() uses the SLOT
    // symbol, not the hop-local one).
    uint dmrs_symb[4];
};

/// \brief Complex product of two interleaved pairs.
///
/// NOT `a * b`: Metal's vector `operator*` is COMPONENT-WISE, so `float2 * float2` would give
/// (a.x*b.x, a.y*b.y) - a mistake this file made once, against a C++ reference that used std::complex
/// semantics and therefore agreed with the host while the kernel did not (found by the L1 GPU run).
static inline float2 mmse_cmul(float2 a, float2 b)
{
    return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

/// The parameters as the kernels use them: every field clamped into the compile-time maxima above, so
/// that a caller's mistake cannot become an out-of-range index (let alone a loop that never ends).
struct mmse_sigma2_dims {
    uint nof_dmrs_symb;
    uint nof_layers;
    uint nof_pilots;
    uint nof_v_pilots;
    uint filter_len;
    uint nof_cdm;
};

static inline mmse_sigma2_dims mmse_sigma2_clamp(constant mmse_sigma2_params& p)
{
    mmse_sigma2_dims d;
    d.nof_dmrs_symb = min(p.nof_dmrs_symb, mmse_max_dmrs_symb);
    d.nof_layers    = min(p.nof_layers, mmse_max_layers);
    d.nof_pilots    = min(p.nof_pilots, mmse_max_pilots_symb);
    d.nof_v_pilots  = min(p.nof_v_pilots, mmse_max_v_pilots);
    d.filter_len    = min(p.filter_len, mmse_max_filter_len);
    d.nof_cdm       = min(p.nof_cdm, mmse_max_cdm);
    return d;
}

/// Evaluates the value the host's enlarged array holds at \c j: the pilots in the middle, the
/// extrapolated virtual pilots at either end, and ZERO outside - the host's convolution_same() treats
/// the outer partial-overlap region as zero-padded, and the virtual pilots only cover \c nof_v_pilots
/// of it (nof_v_pilots <= filter_len / 2). \c j may be negative.
/// \note The dimensions are passed BY VALUE: MSL requires an explicit address space on reference
/// parameters, and this struct is a handful of integers.
static inline float2 mmse_sigma2_extended(device const float*      lse,
                                          threadgroup const float* fit,
                                          mmse_sigma2_dims         d,
                                          uint                     i_base,
                                          int                      j)
{
    const int npf = static_cast<int>(d.nof_pilots);
    const int nv  = static_cast<int>(d.nof_v_pilots);
    if ((j >= nv) && (j < nv + npf)) {
        const ulong src = static_cast<ulong>(i_base) * d.nof_pilots + static_cast<uint>(j - nv);
        return float2(lse[2 * src], lse[2 * src + 1]);
    }
    // Virtual pilot: the same straight line the host fits on the edge window (abs and unwrapped
    // argument), sampled at the requested position. fit[] = {slope_abs, inter_abs, slope_arg,
    // inter_arg} of the start window in [0, 4) and of the end window in [4, 8).
    float x = 0.0F;
    uint  f = 0;
    if (j < nv) {
        x = static_cast<float>(j - nv); // host: i_virtual = i_pilot - nof_v_pilots
        f = 0;
    } else if (j < nv + npf + nv) {
        x = static_cast<float>(j - npf); // host: i_virtual = i_pilot + nof_v_pilots
        f = 4;
    } else {
        return float2(0.0F, 0.0F); // outside the enlarged array: zero padding
    }
    const float rho   = fit[f + 0] * x + fit[f + 1];
    const float phase = fit[f + 2] * x + fit[f + 3] + ((rho > 0.0F) ? 0.0F : 3.14159265358979323846F);
    return float2(cos(phase), sin(phase)) * fabs(rho);
}

/// \brief Frequency-domain smoothing of one (symbol, layer): the virtual pilots of both edges plus the
/// raised-cosine convolution, exactly as apply_fd_smoothing(...strategy=filter...) does it.
///
/// One threadgroup per (symbol, layer), each with mmse_smooth_tg_size threads (the host dispatches
/// exactly that many). The filter coefficients come from the host (they depend only on the hop's
/// geometry), so nothing of filter_type() has to be reproduced here.
kernel void mmse_pilots_fd_smooth(device const float*          lse      [[buffer(0)]],
                                  device float*                smoothed [[buffer(1)]],
                                  device const float*          filt     [[buffer(2)]],
                                  constant mmse_sigma2_params& p        [[buffer(3)]],
                                  uint                         tid      [[thread_position_in_threadgroup]],
                                  uint                         tgid     [[threadgroup_position_in_grid]])
{
    const mmse_sigma2_dims d      = mmse_sigma2_clamp(p);
    const uint             i_symb = (d.nof_layers != 0u) ? (tgid / d.nof_layers) : 0u;
    const uint             i_lay  = (d.nof_layers != 0u) ? (tgid % d.nof_layers) : 0u;
    if ((i_symb >= d.nof_dmrs_symb) || (i_lay >= d.nof_layers) || (d.nof_pilots == 0u)) {
        return; // before any barrier: nothing below runs for this threadgroup
    }
    const uint i_base = i_symb * d.nof_layers + i_lay;
    const int  nv     = static_cast<int>(d.nof_v_pilots);

    threadgroup float fit[8];

    if (tid == 0) {
        // Least-squares straight line through the edge windows, on |.| and on the unwrapped argument
        // (add_v_pilots() -> compute_v_pilots(), the x axis being the index inside the window).
        for (uint which = 0; which != 2u; ++which) {
            const uint  first     = (which == 0u) ? 0u : (d.nof_pilots - d.nof_v_pilots);
            const float nvf       = static_cast<float>(d.nof_v_pilots);
            const float mean_x    = nvf * (nvf - 1.0F) / 2.0F / nvf;
            const float norm_x_sq = (nvf - 1.0F) * nvf * (2.0F * nvf - 1.0F) / 6.0F;
            const float denom     = norm_x_sq - nvf * mean_x * mean_x;
            float       sum_abs   = 0.0F;
            float       sum_arg   = 0.0F;
            float       dot_abs   = 0.0F;
            float       dot_arg   = 0.0F;
            float       arg_prev  = 0.0F;
            float       k_unwrap  = 0.0F;
            for (uint t = 0; t != mmse_max_v_pilots; ++t) {
                if (t >= d.nof_v_pilots) {
                    break;
                }
                const ulong src = static_cast<ulong>(i_base) * d.nof_pilots + first + t;
                const float re  = lse[2 * src];
                const float im  = lse[2 * src + 1];
                const float a   = atan2(im, re);
                if (t != 0u) {
                    const float jump = a - arg_prev;
                    if (fabs(jump) > 3.14159265358979323846F) {
                        k_unwrap -= copysign(1.0F, jump);
                    }
                }
                const float arg = a + 2.0F * k_unwrap * 3.14159265358979323846F;
                arg_prev        = a;
                const float ab  = sqrt(re * re + im * im);
                sum_abs += ab;
                sum_arg += arg;
                dot_abs += ab * static_cast<float>(t);
                dot_arg += arg * static_cast<float>(t);
            }
            const float mean_abs  = sum_abs / nvf;
            const float mean_arg  = sum_arg / nvf;
            const float slope_abs = (denom != 0.0F) ? ((dot_abs - mean_x * mean_abs * nvf) / denom) : 0.0F;
            const float slope_arg = (denom != 0.0F) ? ((dot_arg - mean_x * mean_arg * nvf) / denom) : 0.0F;
            fit[which * 4 + 0]    = slope_abs;
            fit[which * 4 + 1]    = mean_abs - slope_abs * mean_x;
            fit[which * 4 + 2]    = slope_arg;
            fit[which * 4 + 3]    = mean_arg - slope_arg * mean_x;
        }
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    const int npf    = static_cast<int>(d.nof_pilots);
    const int center = static_cast<int>(d.filter_len) / 2;
    for (int m = static_cast<int>(tid); m < npf; m += static_cast<int>(mmse_smooth_tg_size)) {
        float2 acc = float2(0.0F, 0.0F);
        for (uint k = 0; k != mmse_max_filter_len; ++k) {
            if (k >= d.filter_len) {
                break;
            }
            const float2 v = mmse_sigma2_extended(lse, fit, d, i_base, m + nv + center - static_cast<int>(k));
            const float  w = filt[k];
            acc += v * w;
        }
        // The host smooths the pilots the caller has already scaled by 1/beta (see
        // estimate_sigma2(): "The caller has already applied the DM-RS to data scaling (1 / beta) to
        // the LSE pilots"), and the noise estimator then multiplies by beta/nof_dmrs_symb - the two
        // cancel. This kernel reads the UNSCALED device LSE, so the scaling has to happen here or the
        // reconstruction is beta^2 off (measured on the first GPU run: sigma2 7.3% high; the filter is
        // linear, so scaling the output is the same arithmetic as scaling the input).
        device float* dst = smoothed + (static_cast<ulong>(i_base) * d.nof_pilots + static_cast<uint>(m)) * 2;
        dst[0]            = acc.x * p.inv_beta;
        dst[1]            = acc.y * p.inv_beta;
    }
}

/// \brief The classical noise variance of the hop: one threadgroup reduces the whole hop.
///
/// Reproduces estimate_sigma2()'s structure: the layers are processed in CDM PAIRS, each pair gives one
/// noise energy (the difference between the received pilots and the pilots regenerated from the
/// smoothed channel estimates, summed over the DM-RS symbols), and the caller averages the pairs.
kernel void mmse_pilots_sigma2(device const float*          smoothed [[buffer(0)]],
                               device const float*          ref      [[buffer(1)]],
                               device const float*          rx       [[buffer(2)]],
                               device const float*          epochs   [[buffer(3)]],
                               device const float*          cfo      [[buffer(4)]],
                               device float*                out      [[buffer(5)]],
                               constant mmse_sigma2_params& p        [[buffer(6)]],
                               uint                         tid      [[thread_position_in_threadgroup]])
{
    threadgroup float red[mmse_sigma2_tg_size];

    const mmse_sigma2_dims d         = mmse_sigma2_clamp(p);
    const float            scaling   = (d.nof_dmrs_symb != 0u) ? (p.beta / static_cast<float>(d.nof_dmrs_symb)) : 0.0F;
    const uint             nof_pairs = (d.nof_layers + 1u) / 2u;
    float                  sigma2    = 0.0F;

    // Every loop below is bounded by a compile-time constant; the parameters only skip iterations.
    for (uint pair = 0; pair != mmse_max_layers / 2; ++pair) {
        if ((pair >= nof_pairs) || (d.nof_pilots == 0u) || (d.nof_dmrs_symb == 0u)) {
            break;
        }
        const uint l0    = 2u * pair;
        const uint l1    = ((l0 + 2u) < d.nof_layers) ? (l0 + 2u) : d.nof_layers;
        // The received pilots of a pair are those of its CDM group.
        const uint i_cdm = (d.nof_cdm != 0u) ? min(pair, d.nof_cdm - 1u) : 0u;
        const bool pair2 = (l1 - l0) == 2u;

        float energy = 0.0F;
        for (uint i = tid; i < d.nof_pilots; i += mmse_sigma2_tg_size) {
            // scaled[l][i] = SUM over symbols of filtered[s][l][i] * scaling, as the host accumulates it.
            float2 scaled0 = float2(0.0F, 0.0F);
            float2 scaled1 = float2(0.0F, 0.0F);
            for (uint s = 0; s != mmse_max_dmrs_symb; ++s) {
                if (s >= d.nof_dmrs_symb) {
                    break;
                }
                // NOTE the l0 term: the layout is [symbol][layer][pilot], so the pair's first layer is
                // at s * nof_layers + l0 - dropping l0 made every pair but the first read layers 0 and 1
                // (found by the CPU-side comparison of this kernel against the host's estimate_noise()).
                const ulong  base = (static_cast<ulong>(s) * d.nof_layers + l0) * d.nof_pilots + i;
                const float2 f0   = float2(smoothed[2 * base], smoothed[2 * base + 1]) * scaling; // layer l0
                scaled0           = (s == 0u) ? f0 : (f0 + scaled0);
                if (pair2) {
                    const float2 f1 = float2(smoothed[2 * (base + d.nof_pilots)],
                                             smoothed[2 * (base + d.nof_pilots) + 1]) *
                                      scaling;
                    scaled1 = (s == 0u) ? f1 : (f1 + scaled1);
                }
            }
            for (uint s = 0; s != mmse_max_dmrs_symb; ++s) {
                if (s >= d.nof_dmrs_symb) {
                    break;
                }
                const ulong ib = (static_cast<ulong>(s) * d.nof_layers + l0) * d.nof_pilots + i;
                // The phasor is applied to EACH layer's regenerated pilots before they are summed
                // (compensate_cfo_and_accumulate()/estimate_noise() both rotate per layer).
                float2 ph = float2(1.0F, 0.0F);
                if (p.compensate_cfo != 0u) {
                    // Clamped like every other index: the slot symbol index comes from the caller, and
                    // an out-of-range one must read the wrong epoch, not memory outside the buffer.
                    const uint  i_slot = min(p.dmrs_symb[s], mmse_max_slot_symb - 1u);
                    const float theta  = 6.283185307179586F * epochs[i_slot] * cfo[0];
                    ph                 = float2(cos(theta), sin(theta));
                }
                float2 predicted = mmse_cmul(float2(ref[2 * ib], ref[2 * ib + 1]), scaled0);
                predicted        = float2(predicted.x * ph.x - predicted.y * ph.y,
                                          predicted.x * ph.y + predicted.y * ph.x);
                if (pair2) {
                    float2 p1 = mmse_cmul(float2(ref[2 * (ib + d.nof_pilots)], ref[2 * (ib + d.nof_pilots) + 1]),
                                          scaled1);
                    p1        = float2(p1.x * ph.x - p1.y * ph.y, p1.x * ph.y + p1.y * ph.x);
                    // The host accumulates the pair as (second layer) + (first layer).
                    predicted = p1 + predicted;
                }
                const ulong  irx = (static_cast<ulong>(s) * d.nof_cdm + i_cdm) * d.nof_pilots + i;
                const float2 r   = float2(rx[2 * irx], rx[2 * irx + 1]);
                const float2 n   = r - predicted;
                energy += n.x * n.x + n.y * n.y;
            }
        }
        red[tid] = energy;
        threadgroup_barrier(mem_flags::mem_threadgroup);
        for (uint step = mmse_sigma2_tg_size / 2; step != 0u; step /= 2u) {
            if (tid < step) {
                red[tid] += red[tid + step];
            }
            threadgroup_barrier(mem_flags::mem_threadgroup);
        }
        if (tid == 0u) {
            const float total = red[0];
            const bool  ok    = isfinite(total) && (total != 0.0F);
            sigma2 += ok ? (total / static_cast<float>(d.nof_pilots * d.nof_dmrs_symb * (l1 - l0))) : 0.0F;
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }

    if (tid == 0u) {
        out[0] = (nof_pairs == 0u) ? 0.0F : (sigma2 / static_cast<float>(nof_pairs));
    }
}
