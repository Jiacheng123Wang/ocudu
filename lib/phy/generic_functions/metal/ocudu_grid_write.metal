// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// Grid write of one OFDM symbol of one port (FFT phase 2): takes the transform output the DFT kernel
/// just produced and writes the subcarriers the resource grid keeps, as cbf16.
///
/// The dispatch is encoded in the SAME command buffer as the transform (with a buffer barrier between
/// them), so filling the grid from the device costs one dispatch instead of one command buffer round
/// trip per symbol. The grid storage is the buffer the CPU reads afterwards, so nothing is copied back.
///
/// The arithmetic reproduces the host post-processing bit for bit: the same two complex products in the
/// same order (ocudu::ocuduvec::sc_prod with the per-symbol coefficient, then ocudu::ocuduvec::prod
/// with the window table) and the same round-half-to-even conversion to bfloat16 as ocudu::to_bf16().
/// Every product is written as its own statement so the compiler does not contract a multiply and an
/// add into a fused multiply-add: the CPU reference (ARM NEON) rounds after each product as well.

#include <metal_stdlib>
using namespace metal;

struct grid_write_params {
  uint  n;            // Transform size (2^k * 3^m).
  uint  nof_subc;     // Grid subcarriers to write.
  uint  dst_offset;   // Element offset of this (port, symbol) within the grid.
  uint  map_offset;   // grid[i] <- transform[(i + map_offset) % n].
  float phase_re;     // Per-symbol compensation (phase compensation * scaling), real part.
  float phase_im;     // ... imaginary part.
  uint  apply_window; // 1 = multiply by the per-element table.
};

/// Round-to-nearest-even conversion to bfloat16, bit-identical to ocudu::to_bf16() (the 16 least
/// significant fraction bits are dropped, the remaining 7 are rounded half to even).
inline ushort ocudu_to_bf16(float value)
{
  const uint bits = as_type<uint>(value);
  return static_cast<ushort>((bits + 0x7fffu + ((bits >> 16) & 1u)) >> 16);
}

kernel void grid_write(device const float2* fft_out [[buffer(0)]], // transform output of the slot
                       device ushort*       grid    [[buffer(1)]], // grid storage (cbf16 pairs)
                       device const float2* window  [[buffer(2)]], // per-element table (n entries)
                       constant grid_write_params& p [[buffer(3)]],
                       uint tid [[thread_position_in_grid]])
{
  if (tid >= p.nof_subc) {
    return;
  }

  // Upper/lower band mapping: a rotation of the transform output.
  const uint   src = (tid + p.map_offset) % p.n;
  const float2 v   = fft_out[src];

  // v * coefficient (complex product, no contraction: see the file comment).
  const float rr = v.x * p.phase_re;
  const float ii = v.y * p.phase_im;
  const float ri = v.x * p.phase_im;
  const float ir = v.y * p.phase_re;
  float       re = rr - ii;
  float       im = ri + ir;

  if (p.apply_window != 0u) {
    // ... times the window table entry.
    const float2 w  = window[src];
    const float  wr = re * w.x;
    const float  wi = im * w.y;
    const float  vr = re * w.y;
    const float  vi = im * w.x;
    re              = wr - wi;
    im              = vr + vi;
  }

  const uint dst = 2u * (p.dst_offset + tid);
  grid[dst]      = ocudu_to_bf16(re);
  grid[dst + 1u] = ocudu_to_bf16(im);
}
