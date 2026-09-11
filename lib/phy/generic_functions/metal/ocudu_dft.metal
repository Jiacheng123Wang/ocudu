// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// Iterative radix-2 DIT FFT over complex float pairs (float2, byte-compatible
/// with the host cf_t = std::complex<float>). One threadgroup per transform,
/// N <= 4096 (the threadgroup memory budget). Each thread owns N/threads
/// elements at stride threads. Butterfly stages whose distance is smaller than
/// the thread count exchange through the threadgroup buffer with two barriers
/// per stage (read the old pair -> barrier -> write the new pair -> barrier);
/// larger distances stay within one thread's element set and need no
/// synchronization.

#include <metal_stdlib>
using namespace metal;

constant uint MAX_FFT_N = 4096;

kernel void dft_dit(device const float2* in      [[buffer(0)]],
                    device float2*       out     [[buffer(1)]],
                    device const float2* twiddle [[buffer(2)]], // N/2 entries: exp(-2*pi*i*k/N)
                    constant uint&       log2_n  [[buffer(3)]],
                    constant uint&       inverse [[buffer(4)]], // 1 = conjugate twiddles
                    uint                 tid     [[thread_position_in_threadgroup]])
{
    threadgroup float2 buf[MAX_FFT_N];

    const uint n       = 1u << log2_n;
    const uint threads = min(n, 1024u);
    const uint groups  = n / threads;

    if (tid >= threads) {
        return;
    }

    // Bit-reversed load (one element per (group, thread) pair).
    for (uint g = 0; g != groups; ++g) {
        const uint i = g * threads + tid;
        uint       rev = 0;
        for (uint b = 0; b != log2_n; ++b) {
            rev = (rev << 1u) | ((i >> b) & 1u);
        }
        buf[i] = in[rev];
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    // DIRECT = exp(+2*pi*i*k/N) twiddles (matches the CPU generic/FFTZ reference
    // convention); INVERSE conjugates them. Empirically validated against the generic
    // reference by the random-IQ A/B test (the opposite mapping shows up as a ~3 dB
    // conjugation mismatch).
    const float tw_sign = (inverse != 0u) ? -1.0f : 1.0f;

    for (uint s = 1; s <= log2_n; ++s) {
        const uint mh       = 1u << (s - 1u);
        const uint tw_shift = log2_n - s;

        if (mh < threads) {
            // Cross-thread butterflies: the pair spans two threads of the same group.
            // Read the old pair first, barrier, write the new values, barrier.
            float2 own[4];
            float2 partner[4];
            for (uint g = 0; g != groups; ++g) {
                const uint i = g * threads + tid;
                own[g]       = buf[i];
                partner[g]   = buf[i ^ mh];
            }
            threadgroup_barrier(mem_flags::mem_threadgroup);
            for (uint g = 0; g != groups; ++g) {
                const uint i = g * threads + tid;
                const uint k = i & (mh - 1u);
                const float2 w = twiddle[k << tw_shift];
                const float wi = tw_sign * w.y;
                if ((i & mh) == 0u) {
                    // Low element: new = own + w * partner.
                    buf[i] = float2(own[g].x + w.x * partner[g].x - wi * partner[g].y,
                                    own[g].y + w.x * partner[g].y + wi * partner[g].x);
                } else {
                    // High element: new = partner - w * own.
                    buf[i] = float2(partner[g].x - (w.x * own[g].x - wi * own[g].y),
                                    partner[g].y - (w.x * own[g].y + wi * own[g].x));
                }
            }
            threadgroup_barrier(mem_flags::mem_threadgroup);
        } else {
            // Intra-thread butterflies: the pair lives inside this thread's element set.
            for (uint g = 0; g != groups; ++g) {
                const uint i = g * threads + tid;
                if ((i & mh) != 0u) {
                    continue; // each pair is processed by its low element
                }
                const uint k = i & (mh - 1u);
                const float2 w = twiddle[k << tw_shift];
                const float wi = tw_sign * w.y;
                const float2 a = buf[i];
                const float2 b = buf[i + mh];
                buf[i]      = float2(a.x + w.x * b.x - wi * b.y, a.y + w.x * b.y + wi * b.x);
                buf[i + mh] = float2(a.x - (w.x * b.x - wi * b.y), a.y - (w.x * b.y + wi * b.x));
            }
            // No barrier needed (intra-thread); the next cross-thread stage reads after
            // its own read-phase barrier.
        }
    }

    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint g = 0; g != groups; ++g) {
        const uint i = g * threads + tid;
        out[i] = buf[i];
    }
}
