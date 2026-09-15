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
