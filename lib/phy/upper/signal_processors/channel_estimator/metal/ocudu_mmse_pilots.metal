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

/// ---- STATUS (S-7f-5c): kernels written and building, NOT yet dispatched ----
/// Nothing calls these three kernels yet and the estimator still computes the pilots on the host, so
/// the metallib change is behaviour-neutral (the capture gates are unchanged). Known gaps before the
/// switch can be made, each of which has to be settled against the host code it mirrors:
///   1. `compute_hop_submit()` calls the pre-stage ONCE PER HOP (hop 0 and hop 1 of a frequency-hopping
///      allocation), and `cfo_normalized` averages the two hops' CFOs - the per-hop split is the
///      caller's and is not modelled here;
///   2. `td_interpolation_strategy` selects where the symbol-1 products live (pilot_products vs
///      pilots_lse[1]) and whether the layers are averaged (`average_pairs`); only the
///      non-averaging branch's buffers are assumed here;
///   3. EPRE is a reduction over the extracted pilots that the host still owns (and `rx_pilots` is
///      consumed by K4 as well), so the extraction kernel currently duplicates work rather than
///      replacing it;
///   4. which PRBs the hop occupies (`first_prb`/`nof_prb`) and the per-symbol slots come from
///      `extract_common_pattern()` + the layer's `rb_mask`/`rb_mask2`, which the caller must pass.
/// The next step is the wiring plus a tolerance probe against the host's `pilots_lse`, then the OTA
/// handover described in the design document.

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

/// \brief Compensates the CFO on the first two DM-RS symbols' LSE pilots.
///
/// Reproduces compensate_cfo_and_accumulate()'s phasors:
///     lse[0] *= polar(1, -2pi . epoch[dmrs_0] . cfo)
///     lse[1] *= polar(1, -2pi . epoch[dmrs_1] . cfo)
/// (The host applies the second one to the symbol-1 products, which is the same buffer here.)
/// Symbols beyond the first two are left alone, as on the host.
kernel void mmse_pilots_apply_cfo(device float*                lse    [[buffer(0)]],
                                  device const float*          cfo    [[buffer(1)]], // [1]
                                  device const float*          epochs [[buffer(2)]],
                                  constant mmse_pilots_params& p      [[buffer(3)]],
                                  uint2                        gid    [[thread_position_in_grid]])
{
    if ((gid.y >= 2) || (gid.x >= p.nof_layers * p.nof_pilots)) {
        return;
    }
    const float  theta = -6.283185307179586F * epochs[p.dmrs_symb[gid.y]] * cfo[0];
    const float2 ph    = float2(cos(theta), sin(theta));

    const ulong base = (static_cast<ulong>(gid.y) * p.nof_layers * p.nof_pilots) * 2 + static_cast<ulong>(gid.x) * 2;
    const float2 v   = float2(lse[base], lse[base + 1]);
    lse[base]     = v.x * ph.x - v.y * ph.y;
    lse[base + 1] = v.x * ph.y + v.y * ph.x;
}
