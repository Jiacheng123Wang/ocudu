// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief The pilots' mean power and the noise-to-pilot-power ratio: the ONE kernel of K0-a that has
/// a bit-exactness contract against a host float expression.
///
/// ---- Why this kernel lives in its own file ----
/// \c mmse_pilots_power computes
///
///     sigma2_rel = out[0] / max(out[1] / nof_power_pilots, 1e-30F)
///
/// and \c sigma2_rel is the diagonal loading of A in the correlation model. A's condition number is
/// ~2e4, so - exactly like the correlation kernels in ocudu_mmse_corr.metal - this is NOT a quantity
/// whose last bit is irrelevant: a differently rounded quotient changes the published estimate and the
/// receiver's decisions.
///
/// Metal's default fast math rounds this divide differently from the host's in 298151 of 2^20 random
/// (sigma2-like, power-like) pairs (28.4%, measured, both directions), which flipped LLR decisions on
/// 27 of 27 corpus captures (32613 soft bits). Compiled with -fno-fast-math the same sweep is 0 of
/// 2^20: on this GPU the flag is the whole difference, not the hardware.
///
/// The flag is per FILE, so the kernel has to be alone here. Putting the whole of
/// ocudu_mmse_pilots.metal under it would have re-rounded the LSE and the power sum as well - which
/// those kernels have no contract for (the pilots enter h = W . y linearly), and which moved the
/// published dumps on 27 of 27 captures for no benefit. This file is therefore the smallest possible
/// strict unit: one kernel, its parameter struct and the clamps that bound it.
///
/// \warning Anything added to THIS file inherits the strict contract. Anything that needs the ratio
/// must call this kernel, not re-derive the divide somewhere else.

#include <metal_stdlib>
using namespace metal;

/// Compile-time bounds of the kernels: every parameter is clamped into them, so that a caller's
/// mistake cannot become an out-of-range index (let alone a loop that never ends). They mirror the
/// capacities of the buffers the engine binds (mmse_sigma2_tg_size is the reduction's threadgroup).
constant uint mmse_sigma2_tg_size  = 256;
constant uint mmse_max_dmrs_symb   = 4;
constant uint mmse_max_layers      = 4;
constant uint mmse_max_cdm         = 2;
constant uint mmse_max_v_pilots    = 12;
constant uint mmse_max_filter_len  = 31;
constant uint mmse_max_pilots_symb = 3324;

struct mmse_sigma2_params {
    uint  nof_dmrs_symb; // DM-RS symbols of the hop
    uint  nof_layers;    // Tx layers
    uint  nof_pilots;    // pilots per (symbol, layer)
    uint  nof_v_pilots;  // virtual pilots per edge (at most 12)
    uint  filter_len;    // raised-cosine filter length (odd, at most 31)
    uint  nof_cdm;       // CDM groups (the received pilots are indexed by group)
    // Number of pilots the mean power of mmse_pilots_power divides its sum by, i.e. the host's
    // nof_power_pilots (nof_dmrs_symb * nof_layers * nof_pilots, in that order of operations). Also
    // the switch that makes that kernel write the noise-to-pilot-power ratio into out[2]: zero means
    // "the host has no ratio to ask for" (a hop without pilots), and the ratio is then left at zero -
    // which is the value the host's own `(nof_power_pilots == 0) ? 0.0F : ...` produces.
    uint  nof_power_pilots;
    uint  compensate_cfo;
    float beta;          // DM-RS to data amplitude scaling
    float inv_beta;      // 1 / beta: the caller scales the pilots by it BEFORE smoothing (see below)
    // Slot symbol index of each hop DM-RS symbol (the CFO phasor of estimate_noise() uses the SLOT
    // symbol, not the hop-local one).
    uint dmrs_symb[4];
};
// The host mirrors this layout in mmse_sigma2_params_t (ocudu_metal_mmse_engine.mm) and passes it with
// setBytes, so a field added on one side only would silently shift every field after it - and so does
// a same-size swap, which the size assert below cannot see. The host pins the offsets for that reason.
static_assert(sizeof(mmse_sigma2_params) == 56, "mmse_sigma2_params must stay in step with its host mirror");


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

/// \brief Mean-power reference of the hop: one threadgroup reduces the whole hop's LS pilots.
///
/// The estimator used to run this reduction on the host (pilots_power), reading the least-squares
/// pilots back out of the device through ls_pilot() - a host pass over received data whose only
/// consumer is the noise-to-signal ratio sigma2 / pilots_power that the correlation model needs. The
/// same command buffer that produces the pilots and sigma2 now also produces their power sum, so the
/// host reads two scalars and divides them.
///
/// out[0] carries sigma2 (written by mmse_pilots_sigma2, this kernel writes out[1]); the caller reads
/// both after the command buffer completes. The traversal covers exactly the elements the host loop
/// covered - nof_dmrs_symb x nof_layers x nof_pilots of the [symb][layer][pilot] layout - in a
/// different order, which is why the sum can differ from the host's in the last bits: measured over
/// the capture corpus, a relative change of 1e-6 in sigma2_rel does not alter a single published byte
/// (1e-5 does), and the two sums differ by ~1e-7.
///
/// out[2] carries the noise-to-pilot-power ratio sigma2_rel, the one value every host consumer of
/// this stage actually wants: A's diagonal loading (the correlation kernel) and the statistics
/// provider both take the ratio, not either scalar. It is computed here in the host's own two
/// operations on the host's own two operands, and it IS bit-identical to the host's quotient.
///
/// \warning That bit-identity depends on THIS FILE being compiled with -fno-fast-math (see
/// CMakeLists.txt): the ratio is a diagonal loading of A, so it carries cond_2(A) ~ 2e4 of
/// amplification, and under Metal's default fast math this divide comes out differently rounded from
/// the host's in 298151 of 2^20 random (sigma2-like, power-like) pairs (28.4%, both directions) -
/// measured, S-7g-20 - which flipped LLR decisions on 27 of 27 corpus captures. With the flag the
/// same sweep is 0 of 2^20. Do not move this expression to a fast-math source, and measure any new
/// float expression on this path (the host's OCUDU_CE_K0A_RATIO_CHECK=1 probe prints both quotients
/// per hop) rather than arguing it.
///
/// \note The hardware is not the problem: the same divide on the same operands is correctly rounded
/// once the compiler is not allowed to reassociate it.
kernel void mmse_pilots_power(device const float*          lse [[buffer(0)]],
                              device float*                out [[buffer(1)]],
                              constant mmse_sigma2_params& p   [[buffer(2)]],
                              uint                         tid [[thread_position_in_threadgroup]])
{
    threadgroup float red[mmse_sigma2_tg_size];

    const mmse_sigma2_dims d = mmse_sigma2_clamp(p);
    const ulong nof_entries  = static_cast<ulong>(d.nof_dmrs_symb) * d.nof_layers * d.nof_pilots;

    float sum = 0.0F;
    for (ulong i = tid; i < nof_entries; i += mmse_sigma2_tg_size) {
        const float re = lse[2 * i];
        const float im = lse[2 * i + 1];
        sum += re * re + im * im;
    }

    red[tid] = sum;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint stride = mmse_sigma2_tg_size / 2; stride != 0; stride >>= 1) {
        if (tid < stride) {
            red[tid] += red[tid + stride];
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    if (tid == 0) {
        out[1] = red[0];
        // The host's two operations, in the host's order (the sum is divided by the count, not
        // reordered into a mean first). out[3] carries the mean, which is what makes a future
        // discrepancy attributable to the quotient or to an operand.
        if (p.nof_power_pilots != 0u) {
            const float mean = out[1] / static_cast<float>(p.nof_power_pilots);
            const float den  = (mean < 1e-30F) ? 1e-30F : mean;
            out[2]           = out[0] / den;
            out[3]           = mean;
        } else {
            out[2] = 0.0F;
            out[3] = 0.0F;
        }
    }
}
