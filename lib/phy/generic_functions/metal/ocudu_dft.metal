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

/// The optional grid write ("FFT phase 2") is part of this kernel on purpose: it consumes the
/// transform output of this very threadgroup, so the write needs no cross-dispatch dependency - and
/// with it no memory barrier, which is what the sync model would require between two dispatches of
/// one encoder. Cost measured warm: the same as the plain store (see dft_processor_metal_unit_test).
/// With active = 0 the final store is exactly what it always was.
///
/// The input may also be the radio's own int16 buffer (see input_params): the cyclic-prefix skip and
/// the int16 -> float2 scaling the host used to do per symbol happen HERE instead, so the host never
/// touches the samples. The arithmetic is the same single multiply per component ocuduvec::convert()
/// applies on the host (float(sample) * gain with gain = 1 / 32767), which is what keeps the
/// transform output bit-identical to the staged path.

/// Input source of the transform (see ocudu::dft_grid_write_params::time_samples).
struct input_params {
  uint  is_ci16; // 1 = read the int16 pairs at in16[], 0 = read the caller's float2 at in[]
  uint  offset;  // first int16 pair of this transform, relative to the wrapped allocation
  float gain;    // per-component scale of the int16 input
  uint  pad;
};

#include <metal_stdlib>
using namespace metal;

constant uint MAX_FFT_N = 4096;

/// Parameters of the optional resource grid write of this transform (see ocudu::dft_grid_write_params).
struct grid_write_params {
  uint  active;       // 1 = write the grid instead of the plain output buffer
  uint  nof_subc;     // grid subcarriers written by this transform (even)
  uint  dst_offset;   // element offset of this (port, symbol) within the grid
  uint  map_offset;   // unused by the fused form; kept for the standalone layout (see the kernel)
  float phase_re;     // per-symbol compensation (phase compensation * scaling), real part
  float phase_im;     // ... imaginary part
  uint  apply_window; // 1 = multiply by the per-element table
  uint  pad;
};

/// Round-to-nearest-even conversion to bfloat16, bit-identical to ocudu::to_bf16() (the 16 least
/// significant fraction bits are dropped, the remaining 7 are rounded half to even).
inline ushort ocudu_to_bf16(float value)
{
  const uint bits = as_type<uint>(value);
  return static_cast<ushort>((bits + 0x7fffu + ((bits >> 16) & 1u)) >> 16);
}

/// One transform input element: the caller's complex float, or two int16 read straight from the
/// radio buffer and scaled exactly like the host's ocuduvec::convert() does it.
inline float2 ocudu_load_input(device const float2* in,
                               device const short2* in16,
                               constant input_params& ip,
                               uint                 index)
{
  if (ip.is_ci16 == 0u) {
    return in[index];
  }
  const short2 raw = in16[ip.offset + index];
  return float2(static_cast<float>(raw.x) * ip.gain, static_cast<float>(raw.y) * ip.gain);
}

/// \brief Writes transform element \c v (index \c i of the transform) into the resource grid, with the same
/// compensation and rounding the host post-processing applies (ocudu::ocuduvec::sc_prod with the per-symbol
/// coefficient, then ocudu::ocuduvec::prod with the window table, then the bf16 conversion).
///
/// Every product is its own statement so the compiler does not contract a multiply and an add into a fused
/// multiply-add: the CPU reference (ARM NEON) rounds after each product as well.
inline void ocudu_store_grid(device ushort*       grid,
                            device const float2* window,
                            constant grid_write_params& gw,
                            uint                 i,
                            uint                 grid_index,
                            float2               v)
{
  const float rr = v.x * gw.phase_re;
  const float ii = v.y * gw.phase_im;
  const float ri = v.x * gw.phase_im;
  const float ir = v.y * gw.phase_re;
  float       re = rr - ii;
  float       im = ri + ir;

  if (gw.apply_window != 0u) {
    const float2 w  = window[i];
    const float  wr = re * w.x;
    const float  wi = im * w.y;
    const float  vr = re * w.y;
    const float  vi = im * w.x;
    re              = wr - wi;
    im              = vr + vi;
  }

  const uint dst = 2u * (gw.dst_offset + grid_index);
  grid[dst]      = ocudu_to_bf16(re);
  grid[dst + 1u] = ocudu_to_bf16(im);
}

kernel void dft_dit(device const float2* in      [[buffer(0)]],
                    device float2*       out     [[buffer(1)]],
                    device const float2* twiddle [[buffer(2)]], // N/2 entries: exp(-2*pi*i*k/N)
                    device const uint*   perm    [[buffer(3)]], // mixed-radix digit-reversed input index
                    constant uint&       radix2  [[buffer(4)]], // number of radix-2 stages (k)
                    constant uint&       radix3  [[buffer(5)]], // number of radix-3 stages (m)
                    constant uint&       inverse [[buffer(6)]], // 1 = conjugate twiddles
                    constant uint&       base    [[buffer(7)]], // element offset of the first transform
                    device ushort*       grid    [[buffer(8)]], // grid storage (cbf16 pairs), unused when inactive
                    device const float2* window  [[buffer(9)]], // per-element table, unused when inactive
                    constant grid_write_params& gw [[buffer(10)]],
                    device const short2* in16    [[buffer(11)]], // radio samples, unused when is_ci16 = 0
                    constant input_params& ip    [[buffer(12)]],
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
    // Element offset of this transform: the batch base plus one transform per grid position, so a
    // single-transform dispatch can target any slot of the batch buffers (ring usage).
    const uint batch_offset = base + tgid * n;

    if (tid >= threads) {
        return;
    }

    // Twiddle-table lookups below use the N/2-table wrap: the table stores the first N/2
    // roots of unity, and exp(-2*pi*i*(j+N/2)/N) = -exp(-2*pi*i*j/N) for any j >= N/2.

    // Digit-reversed load (one element per owned index; ownership is i = tid, tid+threads, ...).
    for (uint i = tid; i < n; i += threads) {
        buf[i] = ocudu_load_input(in, in16, ip, batch_offset + perm[i]);
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
    if (gw.active != 0u) {
        // Grid write of the (port, symbol) the caller selected. The host maps grid subcarrier g to transform element
        // (g + map_offset) % n with map_offset = n - nof_subc / 2; inverted, the transform elements that belong to the
        // grid are [0, nof_subc / 2) -> grid [nof_subc / 2, nof_subc) and [n - nof_subc / 2, n) -> grid [0, nof_subc / 2),
        // which is what the two conditions below select (nof_subc is even: its width is a whole number of PRBs).
        // (`half` is a Metal type name, hence half_subc.)
        const uint half_subc = gw.nof_subc >> 1u;
        for (uint i = tid; i < n; i += threads) {
            if (i < half_subc) {
                ocudu_store_grid(grid, window, gw, i, half_subc + i, buf[i]);
            } else if (i + half_subc >= n) {
                ocudu_store_grid(grid, window, gw, i, i - (n - half_subc), buf[i]);
            }
        }
    } else {
        for (uint i = tid; i < n; i += threads) {
            out[batch_offset + i] = buf[i];
        }
    }
}
