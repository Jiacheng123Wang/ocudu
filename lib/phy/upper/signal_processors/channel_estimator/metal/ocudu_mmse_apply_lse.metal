// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// =====================================================================================================
// K2-LSE (lever C of dev doc 6.61, S-7f-5y): h = W . y with the pilot rows read straight out of the
// least-squares pilots, so the pilot scatter's dispatch disappears
// =====================================================================================================
//
// WHAT THE SCATTER DID, and why K2 can do it instead. mmse_pilots_scatter_y() (ocudu_mmse_pilots.metal)
// re-indexes K0-a's least-squares pilots into the block layout the weights read:
//
//     y[(i_layer * n_blk_slots + b) * 2 * Ls + 2 * (i_symb * npf + j)]
//         = lse[2 * ((i_symb * nof_layers + i_layer) * nof_pilots + pilot_base + b * npf + j)] * inv_beta
//
// plus two regions it ZEROES: the rows past the group's own pilot count (i_symb * npf + j >= nof_symb *
// npf) and the block slots the group does not fill (b >= n_blk_real). It is a pure re-index and one
// single-precision product per component, so the reader can compute the same value at the same place -
// and then a dispatch per staged group goes away (one per hop without an edge block, two with one).
//
// ---- WHY ITS OWN FILE, AND WHY -fno-fast-math (the measurement that put it here) -------------------
// The product must be rounded where the scatter rounded it: t = lse * inv_beta first, THEN w * t. Under
// Metal's default fast math the compiler is free to reassociate the chain, and it does: the first build
// of this kernel (with mmse_apply, under fast math) came out of the compiler as
//
//     %132 = fmul fast float %123, %106     ; w[k] * inv_beta  <- HOISTED past the LSE load
//     %133 = fmul fast float %132, %127     ; (w * inv_beta) * lse_re
//     %134 = fadd fast float %133, %117     ; + acc
//
// i.e. round(round(w * inv_beta) * lse) instead of round(round(lse * inv_beta) * w). A different
// rounding is a different float, and it was visible exactly where a 1-ulp difference shows and a
// decision-level one does not: _h.bin (cbf16), _llr.bin and the grid stayed byte-identical over the
// 27-capture corpus, while noise_variance and rsrp - float32 reductions over h, in K4 and K5 - moved in
// their last digits on 13 captures (40 bytes of _ce.txt). The dev doc's invariant for this lever is
// "the dumps are byte-identical", so the reassociation had to go, not the criterion.
//
// -fno-fast-math is the flag that removes it, and it is a PER-FILE flag, which is why this kernel is a
// file of its own: ocudu_mmse_apply.metal keeps the default fast math and mmse_apply()'s code
// generation is therefore untouched by this route - "the y route still publishes yesterday's bytes" is
// then a property of the source and not of a measurement. The flag is also value-neutral for the other
// kernel (measured before this file existed: the whole corpus replayed with mmse_apply.metal compiled
// strict against the same file compiled fast - 135 dump files, 0 differing bytes), and what it removes
// here is visible in the IR of the kernel below: two separate fmuls with no fast flags.

#include <metal_stdlib>
using namespace metal;

/// The batch geometry, byte for byte mmse_apply_params in ocudu_mmse_apply.metal (and
/// mmse_apply_params_t on the host): the two kernels are dispatched with the same struct, and a drift
/// between them would be read as geometry.
struct mmse_apply_lse_batch {
    uint nout;        // output positions per block, <= 504
    uint L;           // pilot rows per block slot (the engine's L)
    uint nof_systems; // systems of the batch, <= 8
    uint nof_blocks;  // block slots per system
};

/// One staged GROUP's source geometry, field for field the ones mmse_scatter_params carries (see
/// ocudu_mmse_pilots.metal) minus the y destination, which this route does not use.
struct mmse_y_source {
    uint  sys_lo;       // first system of the group inside THIS batch (the scatter's slot offset)
    uint  sys_hi;       // one past its last system: sys_hi - sys_lo == nof_layers
    uint  nof_layers;   // layers of the LSE layout = systems of the group
    uint  nof_pilots;   // pilots per (symbol, layer) of the whole hop
    uint  nof_symb;     // DM-RS symbols of the hop
    uint  pilot_base;   // first pilot of this group inside the hop
    uint  npf;          // pilots one block carries per DM-RS symbol of THIS group
    uint  n_blk_real;   // block slots this group really fills (the rest are the scatter's zeros)
    float inv_beta;     // the DM-RS to data scaling the scatter applied
};

/// Capacity of the table below, matching the host's k_apply_max_sources / k_max_y_scatter: a merged
/// batch stages exactly two groups (the standard blocks and the narrower edge block).
constant uint mmse_apply_max_sources = 2;

struct mmse_lse_params {
    uint          nof_sources;  // 0 would mean "nothing staged"; the host never dispatches this kernel then
    mmse_y_source sources[mmse_apply_max_sources];
};

/// \brief K2 over the LSE itself: h = W . (lse * inv_beta), addressed per staged group.
///
/// Same grid, same output and same accumulation order as mmse_apply(): one threadgroup per (system,
/// block), one thread per output position, h in the [system][block][2 * nout] layout.
///
/// BOUNDS AND TERMINATION (the file's hard rule, see ocudu_mmse_pilots.metal): a kernel that does not
/// finish takes the machine with it, so every loop bound comes from a parameter and every one of them
/// is clamped into the array the host sized before it is used, and no thread returns before the write
/// it owns. The geometry itself is checked on the HOST (nof_symb * npf <= L, pilot_base +
/// n_blk_real * npf <= nof_pilots, the system ranges cover the batch, the walk stays inside the LSE
/// buffer - see build_lse_sources()); what remains here is the arithmetic that keeps a WRONG parameter
/// a wrong number rather than an out-of-bounds read.
kernel void mmse_apply_lse(device const float*             w   [[buffer(0)]],
                           device float*                   h   [[buffer(1)]],
                           device const float*             lse [[buffer(2)]],
                           constant mmse_apply_lse_batch&  p   [[buffer(3)]],
                           constant mmse_lse_params&       s   [[buffer(4)]],
                           uint                            tid  [[thread_position_in_threadgroup]],
                           uint                            tgid [[threadgroup_position_in_grid]])
{
    // Flattened 1D grid: threadgroup = block + sys * nof_blocks (identical to mmse_apply).
    const uint block = tgid % p.nof_blocks;
    const uint sys   = tgid / p.nof_blocks;

    if (sys >= p.nof_systems || block >= p.nof_blocks || tid >= p.nout) {
        return;
    }

    // Which staged group this system belongs to. A system outside every group (a host bug) sums over
    // the "zeroed" rows only, i.e. publishes h = W . 0 - a wrong number, never an out-of-bounds read.
    const uint nof_sources = min(s.nof_sources, mmse_apply_max_sources);
    uint       g           = 0;
    bool       found       = false;
    for (uint i = 0; i < nof_sources; ++i) {
        if ((sys >= s.sources[i].sys_lo) && (sys < s.sources[i].sys_hi)) {
            g     = i;
            found = true;
            break;
        }
    }
    constant mmse_y_source& src = s.sources[g];

    // Rows of one block slot this group fills, clamped so that the row walk cannot leave the [0, L)
    // range the weights and the products are sized for (the host publishes nof_symb * npf <= L).
    const uint npf   = (src.npf <= p.L) ? src.npf : 0u;
    const uint nsymb = (npf != 0u) ? min(src.nof_symb, p.L / npf) : 0u;

    device const float* wp = w + sys * p.nout * p.L + tid * p.L;

    float acc_re = 0.0F;
    float acc_im = 0.0F;
    uint  k      = 0;
    if (found && (block < src.n_blk_real) && (sys >= src.sys_lo) && (sys - src.sys_lo < src.nof_layers)) {
        // Row k = i_symb * npf + j is lse[i_symb][i_layer][pilot_base + block * npf + j]: consecutive
        // rows of one symbol are consecutive pilots, and the symbol step is the whole [layer][pilot]
        // plane. k is ascending on both loops, exactly as mmse_apply's single loop runs it.
        const ulong         i_layer = static_cast<ulong>(sys - src.sys_lo);
        device const float* lp =
            lse + (i_layer * src.nof_pilots + src.pilot_base + static_cast<ulong>(block) * npf) * 2;
        const ulong symb_stride = static_cast<ulong>(src.nof_layers) * src.nof_pilots * 2;
        for (uint i_symb = 0; i_symb < nsymb; ++i_symb) {
            for (uint j = 0; j < npf; ++j, ++k) {
                const float wj = wp[k];
                // THE TWO ROUNDINGS THE SCATTER PERFORMED, in the same order: scale the pilot, then
                // multiply by the weight. This is the expression -fno-fast-math exists to protect.
                const float tr = lp[2 * j] * src.inv_beta;
                const float ti = lp[2 * j + 1] * src.inv_beta;
                acc_re += wj * tr;
                acc_im += wj * ti;
            }
            lp += symb_stride;
        }
    }
    // The rows the scatter zeroed: its y holds 0.0F there, so the term is w * 0 - the same sum for a
    // finite weight, and a non-finite one still reaches h. Rows above nof_symb * npf AND the block
    // slots of a group that fills fewer slots than the batch strides over both arrive here.
    for (; k < p.L; ++k) {
        const float wj = wp[k];
        acc_re += wj * 0.0F;
        acc_im += wj * 0.0F;
    }

    device float* hp = h + (sys * p.nof_blocks + block) * (2 * p.nout);
    hp[2 * tid]      = acc_re;
    hp[2 * tid + 1]  = acc_im;
}
