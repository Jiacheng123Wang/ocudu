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

// The butterflies both this kernel and the time-alignment port's fused kernel run (see the header).
#include "ocudu_dft_butterflies.h"

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

    ocudu_dft_butterflies(buf, n, radix2, radix3, twiddle, tid, threads, tw_sign);

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
