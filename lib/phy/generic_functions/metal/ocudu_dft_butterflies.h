// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// The mixed-radix DIT butterflies, shared by the two kernels that need a transform:
//
//   * `dft_dit` (ocudu_dft.metal) - the DFT engine's kernel, one threadgroup per transform, its input
//     gathered through the permutation table and its output written to the caller's buffer or mapped
//     onto the resource grid;
//   * `mmse_ta_chain` (ocudu_mmse_ta.metal) - the time-alignment port's fused kernel, which places
//     the hop's pilots itself and then accumulates the power delay profile in the same threadgroup
//     (batch 5d: one dispatch instead of three, because the dispatches - not the arithmetic - are what
//     the lane pays for; design doc 17.10.5).
//
// It is a header rather than a comment in one of the two because the metallibs are separate
// translation units: the alternative - a second copy of the butterflies for the fused kernel - would
// be two implementations of one transform, which is exactly the kind of drift this port has spent its
// time hunting (the twiddle convention, the digit-reversal order, the N/2-table wrap).
//
// The caller owns the ORDER: `buf` must already hold the input in the order the first stage needs.
// `dft_dit` gets that from the permutation gather; the fused kernel places the pilots directly (in
// natural order, which is what that gather would have produced from it).

#pragma once

#include <metal_stdlib>
using namespace metal;

/// \brief The mixed-radix DIT butterflies of one transform, in place on a threadgroup buffer.
///
/// Extracted from the kernel below (batch 5d) so that the time-alignment port can run THE SAME
/// transform inside its own fused kernel: it does the whole chain - placement, transform, power delay
/// profile, peak - in one threadgroup, because three dispatches cost the lane 50us/lane while the
/// arithmetic costs nothing (design doc 17.10.5). Sharing this function is what keeps the two from
/// drifting: there is one implementation of the butterflies, called twice.
///
/// \param buf     `n` complex values, already in the order the first stage needs (the caller loads
///                them: dft_dit gathers through the permutation table, the fused kernel places the
///                pilots directly).
/// \param n       transform size, a power of two times a power of three.
/// \param twiddle N/2 roots of unity, exp(-2*pi*i*k/N).
/// \param threads threads in the threadgroup, and the stride the owned elements are walked with.
/// \param tw_sign -1 for the INVERSE direction (the estimator's IDFT), +1 for the direct one.
inline void ocudu_dft_butterflies(threadgroup float2*         buf,
                                  uint                       n,
                                  uint                       radix2,
                                  uint                       radix3,
                                  device const float2*       twiddle,
                                  uint                       tid,
                                  uint                       threads,
                                  float                      tw_sign)
{
    const uint half_n = n >> 1u;

    // Twiddle-table lookups below use the N/2-table wrap: the table stores the first N/2
    // roots of unity, and exp(-2*pi*i*(j+N/2)/N) = -exp(-2*pi*i*j/N) for any j >= N/2.

    // Per-element stage state: at most ceil(4096/1024) = 4 owned elements per thread.
    float2 a[4];
    float2 b[4];
    float2 c[4];
    uint   kpos[4]; // position within the butterfly group (twiddle index base)
    uint   j[4];    // position of the owned element inside the group (0..r-1)

    // ---- Radix-2 stages: butterfly spacing g = 2^s, twiddle W_{2g}^{kpos} ----
    for (uint s = 0; s != radix2; ++s) {
        const uint g    = 1u << s;
        const uint step = n >> (s + 1u); // n / (2g)

        uint e = 0;
        for (uint i = tid; i < n; i += threads, ++e) {
            kpos[e] = i & (g - 1u);
            j[e]    = (i >> s) & 1u;
            a[e]    = buf[i];
            b[e]    = buf[i ^ g];
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
        e = 0;
        for (uint i = tid; i < n; i += threads, ++e) {
            const float2 w = twiddle[kpos[e] * step];
            const float  wi = tw_sign * w.y;
            if (j[e] == 0u) {
                buf[i] = float2(a[e].x + w.x * b[e].x - wi * b[e].y,
                                a[e].y + w.x * b[e].y + wi * b[e].x);
            } else {
                // High element of the pair: new = partner - w * own.
                buf[i] = float2(b[e].x - (w.x * a[e].x - wi * a[e].y),
                                b[e].y - (w.x * a[e].y + wi * a[e].x));
            }
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }

    // ---- Radix-3 stages: combine three sub-DFTs of size g (spacing g = 2^k * 3^t) ----
    // Standard mixed-radix DIT: with B_u[k] = the k-th bin of sub-DFT u and the output index
    // i = u*g + k inside a group of size 3g:
    //   X[i] = sum_t B_t[k] * W_{3g}^{t*k} * W_3^{t*u}.
    uint g = 1u << radix2;
    for (uint t = 0; t != radix3; ++t) {
        // n / (3g) = 3^(m-t-1): the global-twiddle stride of W_{3g}^{k}.
        uint step = 1;
        for (uint q = 0; q + 1u < radix3 - t; ++q) {
            step *= 3u;
        }
        // W_3 = exp(+/-2*pi*i/3) and its square (DIRECT: + exponent, matching the radix-2 stage).
        const float2 w3  = float2(-0.5f, tw_sign * 0.8660254037844386f);
        const float2 w3c = float2(-0.5f, -tw_sign * 0.8660254037844386f);

        uint e = 0;
        for (uint i = tid; i < n; i += threads, ++e) {
            const uint k          = i % g;
            const uint group_base = i - (i % (3u * g));
            kpos[e] = k;
            j[e]    = (i / g) % 3u;
            a[e]    = buf[group_base + k];
            b[e]    = buf[group_base + g + k];
            c[e]    = buf[group_base + 2u * g + k];
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
        e = 0;
        for (uint i = tid; i < n; i += threads, ++e) {
            // Input pre-twiddles: W_{3g}^{k} and W_{3g}^{2k} (with the N/2-table wrap).
            const uint  i1 = kpos[e] * step;
            const uint  i2 = 2u * kpos[e] * step;
            const float2 w1 = (i1 < half_n) ? twiddle[i1]
                                            : float2(-twiddle[i1 - half_n].x, -twiddle[i1 - half_n].y);
            const float2 w2 = (i2 < half_n) ? twiddle[i2]
                                            : float2(-twiddle[i2 - half_n].x, -twiddle[i2 - half_n].y);
            const float  w1i = tw_sign * w1.y;
            const float  w2i = tw_sign * w2.y;
            // A1 = b * W_{3g}^{k}, A2 = c * W_{3g}^{2k}.
            const float2 A1 = float2(w1.x * b[e].x - w1i * b[e].y, w1.x * b[e].y + w1i * b[e].x);
            const float2 A2 = float2(w2.x * c[e].x - w2i * c[e].y, w2.x * c[e].y + w2i * c[e].x);
            // Output twiddles W_3^{t*u}: u = j[e], W_3 = exp(-2*pi*i/3) (w3c) and its
            // square (w3) for the DIRECT convention.
            float2 y;
            if (j[e] == 0u) {
                y = a[e] + A1 + A2;
            } else if (j[e] == 1u) {
                y = float2(a[e].x + (w3c.x * A1.x - w3c.y * A1.y) + (w3.x * A2.x - w3.y * A2.y),
                           a[e].y + (w3c.x * A1.y + w3c.y * A1.x) + (w3.x * A2.y + w3.y * A2.x));
            } else {
                y = float2(a[e].x + (w3.x * A1.x - w3.y * A1.y) + (w3c.x * A2.x - w3c.y * A2.y),
                           a[e].y + (w3.x * A1.y + w3.y * A1.x) + (w3c.x * A2.y + w3c.y * A2.x));
            }
            buf[i] = y;
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
        g *= 3u;
    }

}
