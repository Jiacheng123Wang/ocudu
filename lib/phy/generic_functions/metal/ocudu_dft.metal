// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// Iterative mixed-radix (2^k * 3^m) DIT FFT over complex float pairs (float2,
/// byte-compatible with the host cf_t = std::complex<float>). One threadgroup per
/// transform, N <= 4096 (the threadgroup memory budget). This covers every NR
/// OFDM FFT size of the 2^k * 3^m family (384, 512, 768, 1024, 1536, 2048,
/// 3072, 4096, ...); the PRACH sizes stay on the CPU implementation.
///
/// The host provides the mixed-radix digit-reversed input permutation table
/// (the DIT input order), the twiddle table (N/2 entries of exp(-2*pi*i*k/N))
/// and the radix-2/radix-3 factor counts. Each butterfly stage uses the
/// read-old-pair -> barrier -> write-new-value -> barrier exchange over the
/// threadgroup buffer, so no cross-threadgroup synchronization exists.

#include <metal_stdlib>
using namespace metal;

constant uint MAX_FFT_N = 4096;

kernel void dft_dit(device const float2* in      [[buffer(0)]],
                    device float2*       out     [[buffer(1)]],
                    device const float2* twiddle [[buffer(2)]], // N/2 entries: exp(-2*pi*i*k/N)
                    device const uint*   perm    [[buffer(3)]], // mixed-radix digit-reversed input index
                    constant uint&       radix2  [[buffer(4)]], // number of radix-2 stages (k)
                    constant uint&       radix3  [[buffer(5)]], // number of radix-3 stages (m)
                    constant uint&       inverse [[buffer(6)]], // 1 = conjugate twiddles
                    uint                 tid     [[thread_position_in_threadgroup]],
                    uint                 tgid    [[threadgroup_position_in_grid]])
{
    threadgroup float2 buf[MAX_FFT_N];

    // N = 2^k * 3^m.
    uint n3 = 1;
    for (uint q = 0; q != radix3; ++q) {
        n3 *= 3u;
    }
    const uint n       = (1u << radix2) * n3;
    const uint threads = min(n, 1024u);
    const uint half_n  = n >> 1u;

    // One threadgroup per transform: independent transforms are batched by dispatching several
    // threadgroups, each working on its own slice of the input/output buffers.
    const uint batch_offset = tgid * n;

    if (tid >= threads) {
        return;
    }

    // Twiddle-table lookups below use the N/2-table wrap: the table stores the first N/2
    // roots of unity, and exp(-2*pi*i*(j+N/2)/N) = -exp(-2*pi*i*j/N) for any j >= N/2.

    // Digit-reversed load (one element per owned index; ownership is i = tid, tid+threads, ...).
    for (uint i = tid; i < n; i += threads) {
        buf[i] = in[batch_offset + perm[i]];
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    // DIRECT: w = the raw table value exp(-2*pi*i*k/N) (matches the CPU generic/FFTZ
    // reference convention); INVERSE conjugates it.
    const float tw_sign = (inverse != 0u) ? -1.0f : 1.0f;

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

    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint i = tid; i < n; i += threads) {
        out[batch_offset + i] = buf[i];
    }
}
