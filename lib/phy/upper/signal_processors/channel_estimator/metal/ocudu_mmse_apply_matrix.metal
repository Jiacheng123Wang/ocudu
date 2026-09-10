// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// =============================================================================
// K2-NN (matrix-accelerated variant): h = W . y  per (system, time-frequency block)
// =============================================================================
// A/B comparison implementation (CLI: --pusch_channel_estimator_algo metal_nn_mmse).
// Same math as ocudu_mmse_apply.metal (K2), but the per-block "matrix x complex vector"
// product is restructured into a batched GEMM on the GPU hardware matrix unit:
//   old K2:  per (sys, block): h[nout] = W[nout x L] * y[L]   (complex vector, 2 real
//            matrix-vector products)
//   new K2N: pack 4 blocks into 8 real columns and compute H = W * Y once:
//            (nout x 8) = (nout x L) * (L x 8) as a real GEMM, then unpack the output
//            back to h with the complex real/imag interleaved stride.
// ocudu_mmse_apply.metal is left untouched.
//
// =============================================================================
// ---- 1. Why 4 blocks x 2 (real/imag) = 8 columns: seamless 8x8 hardware matrix ----
// The fp32 hardware matrix primitive only supports 8x8 shapes. Organising the B side
// (the multiplicand) into 8 columns makes one row of an 8x8 B_tile exactly 8 consecutive
// floats (a 32 B aligned burst), so a single hardware matrix load covers one continuous
// 256 B span with no waste.
//
// The complex vectors are stored real/imag interleaved per block (2L floats per block):
//     y_interleaved[gb][2k + e],  e = 0 real / 1 imag,  k = pilot index
//     (k uses the symbol-major order shared with K2 / the correlation build:
//      k = i_dmrs_sym * npf + j_sc)
// Adjacent pilots are 2 floats apart in memory (interleave stride 2), so they cannot be
// used directly as matrix rows with "row stride 8". The host (C++ packing) rearranges the
// 4 blocks into a real matrix Y (row-major):
//     Y[k][2*bl + e] = y_interleaved[quad*4 + bl][2k + e]
// i.e. Y is an Lp x 8 matrix: rows = pilots k (row stride exactly 8 floats, 8 rows = 64
// consecutive floats), columns = (block index bl inside the quad, real/imag part e). All
// pilots of one quad (4 blocks) are therefore exactly one contiguous [Lp][8] region:
//     qy layout [nof_systems][nquads][Lp][8], nquads = ceil(nof_blocks/4),
// so the B_tile load (rows k0..k0+8, 8 columns) has base address qy + (...)*8 + k0*8 with
// row stride 8 and is fully contiguous. In the hardware matrix product C(r, c) = H[o0+r][c]
// the column c = 2*bl + e is naturally "real part (e=0) / imag part (e=1) of position
// o0+r in block bl".
//
// ---- 2. 8-alignment zero-padding contract: any nout/L takes the matrix path ----
//     Lp = ceil8(L),  Np = ceil8(nout)   (L <= 72, nout <= 504, any value allowed)
// The host performs the following clearing while packing, so this kernel has no branch:
//     * W is stored row-major as [nof_systems][Np][Lp] (row stride Lp): its pad rows and
//       columns coming from K1b-NN are always 0 (see the derivation in
//       ocudu_mmse_weights_matrix.metal);
//     * qy is packed with Lp rows: real pilot rows k < L, rows k in [L, Lp) all 0; in the
//       tail quad (nof_blocks % 4 != 0) the columns of the non-existent blocks are all 0.
// Consequently the pad rows contribute exactly 0 to every output element
// (sum_k w[o][k]*Y[k][c] where at least one of w and Y is 0 for k >= L), so the truncated
// result is identical to the unpadded one. The geometric overhead of running the k loop
// Lp/8 instead of L/8 times is part of the A/B performance comparison.
//
// ---- 3. SIMD-group thread mapping and output truncation ----
// Each output row tile (positions o0..o0+8, 8 columns) is handled by one simdgroup
// (32 lanes):  grid = nof_systems * nquads * ceil(nout/8) threadgroups (32 threads each).
// The 64 result elements of a tile live in the 32 lanes' private registers (2 elements per
// lane, two adjacent columns in the same row, i.e. one float2 = (real, imag) pair).
//
// The write-back target is still the legacy complex interleaved layout (block-major, real
// width 2*nout):
//     h[gb][2*o + e],  o = position inside the block,  e = real/imag
// Note the three different physical strides:
//     * adjacent positions o inside a block: stride 2 floats (interleaved)
//     * between blocks gb:                   stride 2*nout floats
// A direct simdgroup_store write-back cannot express this: one store call has a single
// base address and a single element row stride, so it cannot place each lane's float2 at a
// different block base (the block offset 2*nout implied by the column index 2*bl does not
// match the coefficient 2 of the lane column pair c). The standard two-step scheme is used:
//     a) simdgroup_store writes the 8x8 result to the threadgroup scratch ctg[8][8]
//        (row stride 8, contiguous 256 B, done in one shot);
//     b) after threadgroup_barrier, each lane reads back the float2 of its own (row r,
//        column pair 2bl) and writes it to device memory at 8 B granularity:
//        lane = bl*8 + r (0..31)
//        h2[ (sys*nof_blocks + gb) * nout + o ] = (re, im)
//        with gb = quad*4 + bl, o = o0 + r (real position) and h2 the float2 view.
//     The 8 consecutive lanes of the same bl write one contiguous 64 B span, which is
//     write-combining friendly.
//   [Output truncated to the real dims] in a pad row tile every position o >= nout (value
//   always 0) is skipped, because the h buffer only has 2*nout floats per block - this is
//   the only bounds check in this kernel.
//
// ---- 4. Graceful tail handling: nof_blocks not a multiple of 4 ----
// nquads = ceil(nof_blocks/4); the last quad holds only n_tail = nof_blocks % 4 real blocks
// (1..3). The packing side zero-fills the columns of the non-existent blocks (B_tile still
// reads all 8 columns; the products of those columns are 0 and the columns do not pollute
// each other). This kernel checks gb < nof_blocks before writing back, so non-existent
// blocks are computed but not stored. The main path pays no tail branch.
//
// =============================================================================

#include <metal_stdlib>
using namespace metal;

struct mmse_apply_matrix_params {
  uint nout;        // actual output positions per block, <= 504 (any value; kernel pads to ceil8)
  uint L;           // actual pilots per block, <= 72 (any value; kernel pads to ceil8)
  uint nof_systems; // number of systems, <= 8
  uint nof_blocks;  // time-frequency blocks (GPU standard blocks, any positive integer, tail 1..3)
};

kernel void mmse_apply_matrix(device const float* w  [[buffer(0)]], // [nof_systems][Np][Lp] row-major weights (pads = 0)
                              device const float* qy [[buffer(1)]], // [nof_systems][nquads][Lp][8] real Y matrix (host packed, pad rows/cols 0)
                              device float*       h  [[buffer(2)]], // [nof_systems][nof_blocks][2*nout] complex interleaved output (real width)
                              constant mmse_apply_matrix_params& p [[buffer(3)]],
                              uint tgid [[threadgroup_position_in_grid]],
                              uint tid  [[thread_position_in_threadgroup]])
{
  // Padded tiling dims: the weight row stride and the qy row count both follow Lp/Np.
  const uint Lp = (p.L + 7u) & ~7u;   // ceil8(L)
  const uint Np = (p.nout + 7u) & ~7u; // ceil8(nout)

  // ---- grid decomposition: tgid -> (sys, quad, output row tile rt) ----
  const uint nquads = (p.nof_blocks + 3u) >> 2; // 4 blocks are packed into 1 quad
  const uint rtiles = Np >> 3;                  // output row tiles per quad = Np/8
  const uint qrt    = nquads * rtiles;          // tiles per system
  const uint sys    = tgid / qrt;
  const uint rem    = tgid - sys * qrt;
  const uint quad   = rem / rtiles;             // quad index: handles blocks [4*quad, 4*quad+4)
  const uint rt     = rem - quad * rtiles;      // row tile: output positions [8*rt, 8*rt+8)
  const uint o0     = rt << 3;

  // W row-block base (row stride = Lp): this simdgroup handles W rows o0..o0+7 (pad rows are 0)
  device const float* wp = w + sys * (Np * Lp) + o0 * Lp;
  // Y(quad) region base: row-major [Lp][8], row stride = 8 floats (pad rows are 0)
  device const float* yp = qy + (sys * nquads + quad) * (Lp * 8u);

  // ---- Hardware multiply-accumulate: H_tile(8 x 8) = W[o0..o0+8, :] * Y[:, 0..8) ----
  simdgroup_matrix<float, 8, 8> a_tile;
  simdgroup_matrix<float, 8, 8> b_tile;
  simdgroup_matrix<float, 8, 8> acc(0.0F); // diag(0) == all-zero accumulator initial value
  for (uint k0 = 0; k0 < Lp; k0 += 8u) {
    // A_tile(r, k) = w[(o0+r)*Lp + k0+k], row stride Lp
    simdgroup_load(a_tile, wp + k0, Lp, ulong2(0, 0), false);
    // B_tile(k, c) = qy[(k0+k)*8 + c], row stride 8 -- 8 rows, fully contiguous 256 B
    simdgroup_load(b_tile, yp + k0 * 8u, 8u, ulong2(0, 0), false);
    simdgroup_multiply_accumulate(acc, a_tile, b_tile, acc);
  }

  // ---- Complex interleave unpacking: threadgroup staging + float2 scatter (with real-dim truncation) ----
  threadgroup float ctg[8][8]; // 8x8 result scratch (row-major, row stride 8)
  simdgroup_store(acc, &ctg[0][0], 8u, ulong2(0, 0), false);
  threadgroup_barrier(mem_flags::mem_threadgroup);

  // Each lane owns 1 float2 (8 B):  lane 0..31 -> (bl = lane/8, r = lane%8)
  //   bl = block index inside the quad (0..3), r = position row inside the block (0..7)
  // Truncation guard: only real positions are written - out-of-range blocks of the tail
  // quad (gb >= nof_blocks) and positions o >= nout of pad row tiles are skipped (the h
  // buffer uses the real 2*nout layout, so it must not be written out of bounds).
  const uint bl = tid >> 3;
  const uint r  = tid & 7u;
  const uint gb = (quad << 2) + bl; // global block index
  const uint o  = o0 + r;           // real position inside the block
  if (gb < p.nof_blocks && o < p.nout) {
    // h is addressed through the float2 view: block gb has nout float2 entries, position o.
    device float2* hp2 = reinterpret_cast<device float2*>(h) +
                         (sys * p.nof_blocks + gb) * p.nout + o;
    // ctg[r][2*bl], ctg[r][2*bl+1] are exactly the (real, imag) parts of that position.
    hp2[0] = float2(ctg[r][2 * bl], ctg[r][2 * bl + 1]);
  }
}
