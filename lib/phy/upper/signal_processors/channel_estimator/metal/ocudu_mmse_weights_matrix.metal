// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// =============================================================================
// K1b-NN (matrix-accelerated variant): W = R_hp . A^-1  per system
// =============================================================================
// A/B comparison implementation (CLI: --pusch_channel_estimator_algo metal_nn_mmse).
// Same math as ocudu_mmse_weights.metal (K1b), but the compute engine is the Apple GPU
// hardware matrix unit (simdgroup_matrix<float,8,8> + simdgroup_multiply_accumulate).
//
// ---- 8-alignment zero-padding contract: any nout/L takes the matrix path ----
// The fp32 hardware matrix primitive only supports 8x8 shapes, so regardless of whether
// the input dims are multiples of 8 this kernel tiles the padded dims seamlessly:
//     Lp = ceil8(L) = (L + 7) & ~7       (pilot dim, <= 72)
//     Np = ceil8(nout) = (nout + 7) & ~7 (output position dim, <= 504)
// The padding is done by the HOST while packing (not by this kernel): the R_hp / A_inv
// operands and the W output handed to this kernel are stored with row stride Lp and their
// out-of-range rows/columns are already zeroed:
//     R_hp_pad  [nof_systems][Np][Lp]   real values in rows o < nout and cols k < L, else 0
//     A_inv_pad [nof_systems][Lp][Lp]   real inverse in rows/cols < L, else 0
//     W_pad     [nof_systems][Np][Lp]   written fully (pad columns are exactly 0)
// Because the A_inv_pad pad columns are all zero, the W pad columns (= columns >= L of
// R_hp_pad . A_inv_pad) are exactly zero; likewise the R_hp_pad pad rows make the W pad
// rows exactly zero. These two properties guarantee that the downstream apply kernel
// multiplying the zero pad rows of qy contributes nothing, so the truncated result is
// identical to the unpadded one. The padding overhead is purely geometric: the k loop
// runs Lp/8 instead of L/8 iterations and the tile counts come from ceil(), while the pad
// region produces no valid output. That overhead is part of the A/B performance comparison.
//
// ---- SIMD-group thread mapping (one simdgroup = 32 lanes = 1 threadgroup) ----
// fp32 matrix primitive: simdgroup_matrix<float, 8, 8>; each lane holds 2 private float
// registers (float2 access granularity). load/store address elements row-major (verified
// on an M4 Pro in this repository):
//     element(r, c) <-> mem[(origin.y + r) * elements_per_row + origin.x + c]
// Each (sys, row tile, column tile) maps to one 8x8 output sub-block of W_pad and is
// handled by 1 simdgroup: grid = nof_systems * ceil(nout/8) * ceil(L/8) threadgroups
// (32 threads each).
//
// ---- Hardware multiply-accumulate along the inner-product dim (k in steps of 8 over Lp) ----
//   for k0 = 0, 8, ..., Lp-8:
//     A_tile = R_hp_pad[o0..o0+8)[k0..k0+8)   (device load, row stride = Lp)
//     B_tile = A_inv_pad[k0..k0+8)[c0..c0+8)  (device load, row stride = Lp)
//     acc    = acc + A_tile * B_tile
// Both operand tiles are 8 rows x 8 columns with row stride Lp (always a multiple of 8,
// so every row starts 32 B aligned) and each load covers exactly one regular 256 B span.
// The W_pad sub-block is stored back directly with row stride Lp as well.
// =============================================================================

#include <metal_stdlib>
using namespace metal;

struct mmse_weights_matrix_params {
  uint nout;        // actual output positions per block, <= 504 (any value; kernel pads to ceil8)
  uint L;           // actual pilots per block, <= 72 (any value; kernel pads to ceil8)
  uint nof_systems; // number of systems (port x layer x hop), <= 8
};

kernel void mmse_weights_matrix(device const float* r_hp [[buffer(0)]], // [nof_systems][Np][Lp] row-major, pads zeroed
                                device const float* a_inv [[buffer(1)]], // [nof_systems][Lp][Lp] row-major, pads zeroed
                                device float*       w     [[buffer(2)]], // [nof_systems][Np][Lp] row-major (fully written, pad cols = 0)
                                constant mmse_weights_matrix_params& p [[buffer(3)]],
                                uint tgid [[threadgroup_position_in_grid]])
{
  // Padded tiling dims: every division and the whole grid use Lp/Np, so no bounds branch is needed.
  const uint Lp = (p.L + 7u) & ~7u;   // ceil8(L)
  const uint Np = (p.nout + 7u) & ~7u; // ceil8(nout)

  // ---- tgid decomposition: 1D flat grid -> (sys, row tile rt, column tile ct) ----
  const uint nct = Lp >> 3;    // 8x8 tiles along the columns = Lp/8
  const uint nrt = Np >> 3;    // 8x8 tiles along the rows = Np/8
  const uint tpg = nrt * nct;  // tiles per system
  const uint sys = tgid / tpg;
  const uint rem = tgid - sys * tpg;
  const uint rt  = rem / nct;      // covers W_pad rows [8*rt, 8*rt+8)
  const uint ct  = rem - rt * nct; // covers W_pad columns [8*ct, 8*ct+8)
  const uint o0  = rt << 3;
  const uint c0  = ct << 3;

  // Base address of this 8x8 output sub-block (in elements); elements_per_row = Lp.
  device const float* rp = r_hp + sys * (Np * Lp) + o0 * Lp; // A_tile base (+ k0)
  device const float* ap = a_inv + sys * (Lp * Lp) + c0;     // B_tile base (+ k0*Lp)
  device float*       wp = w + sys * (Np * Lp) + o0 * Lp + c0;

  // Hardware matrix register tiles; the scalar constructor yields diag(0) == all-zero (acc init).
  simdgroup_matrix<float, 8, 8> a_tile;
  simdgroup_matrix<float, 8, 8> b_tile;
  simdgroup_matrix<float, 8, 8> acc(0.0F);

  // ---- Hardware multiply-accumulate: step 8 along k (inner product) across Lp ----
  // acc(r, c) = sum_k R_hp_pad[o0+r][k] * A_inv_pad[k][c0+c], k = k0..k0+8.
  // The pad span takes part in the arithmetic but is zero (zeroed by the host), so it
  // contributes nothing.
  for (uint k0 = 0; k0 < Lp; k0 += 8u) {
    // A_tile(r, k) = r_hp[(o0+r)*Lp + k0+k]  <=>  starting at rp[k0], row stride Lp
    simdgroup_load(a_tile, rp + k0, Lp, ulong2(0, 0), false);
    // B_tile(k, c) = a_inv[(k0+k)*Lp + c0+c]  <=>  starting at ap[k0*Lp], row stride Lp
    simdgroup_load(b_tile, ap + k0 * Lp, Lp, ulong2(0, 0), false);
    simdgroup_multiply_accumulate(acc, a_tile, b_tile, acc);
  }

  // Store the whole 8x8 sub-block back to w[(o0+r)*Lp + c0+c] with row stride Lp - the pad
  // positions are written too (always 0); consumers read the real nout/L and truncate, and
  // nothing goes out of bounds (the buffers are allocated for the Np x Lp capacity).
  simdgroup_store(acc, wp, Lp, ulong2(0, 0), false);
}
