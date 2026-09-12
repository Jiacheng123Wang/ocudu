// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// K1: batched matrix inversion for the 2D MMSE channel estimator.
//
// The matrix is A = R_pp + (sigma2 + ridge) I: R_pp is a correlation matrix (positive
// semi-definite) and sigma2 + ridge is strictly positive, so A is SYMMETRIC POSITIVE DEFINITE.
// Gaussian elimination therefore needs NO pivoting - the pivots stay above the smallest
// eigenvalue - which removes the serial pivot scan a naive implementation would need.
//
// Shape: block Gauss-Jordan with 8x8 blocks, one threadgroup per system, laid out as (column,
// row). Each block column [bs, be) is reduced in three steps:
//
//   A. the diagonal block is reduced to the identity with the per-pivot steps (normalize the pivot
//      row, eliminate its column from the other rows OF THE BLOCK): at most 8 pivots over at most
//      8 rows, so with a wide threadgroup every thread touches about one element per pivot;
//   B. the pivot columns of the rows outside the block are saved as the elimination multipliers
//      (they are read from a separate array afterwards, so no thread reads a value another one is
//      writing);
//   C. those pivot columns are zeroed (the block Gauss-Jordan result is exactly zero there) and
//      the remaining columns are updated with one rank-blk sweep.
//
// Compared with the per-pivot form this kernel replaces, the bulk of the matrix is updated once
// per BLOCK column instead of once per single pivot column (36 sweeps -> 5), while the sequential
// part stays inside the 8x8 block. Measured on the production shape (one 36x36 system): 91.3 us
// with (32,4) threads and 33.9 us with (64,16) for the per-pivot form.
//
// Matrix order n <= 36, nof_systems <= 8.

#include <metal_stdlib>
using namespace metal;

constant uint MAX_N  = 36;  // maximum matrix order
constant uint MAX_N2 = 72;  // 2 * MAX_N
constant uint BLK    = 8;   // block size of the elimination

kernel void mmse_inv(device float*       a           [[buffer(0)]],  // [nof_systems][n][n] row-major
                     constant uint&      n           [[buffer(1)]],
                     constant uint&      nof_systems [[buffer(2)]],
                     uint2               tid         [[thread_position_in_threadgroup]],
                     uint2               tgs         [[threads_per_threadgroup]],
                     uint2               tgid        [[threadgroup_position_in_grid]])
{
  if (tgid.x >= nof_systems) {
    return;
  }

  // [A | I] in threadgroup memory; the inverse replaces A in place.
  threadgroup float gj[MAX_N][MAX_N2];
  // Elimination multipliers: the pivot columns of the rows outside the current block.
  threadgroup float mult[MAX_N][BLK];

  device float* src = a + tgid.x * n * n;

  const uint tid_lin = tid.y * tgs.x + tid.x;
  const uint nthr    = tgs.x * tgs.y;

  // Load [A | I]: every thread covers a stripe of the block.
  for (uint r = tid.y; r < n; r += tgs.y) {
    for (uint c = tid.x; c < 2 * n; c += tgs.x) {
      gj[r][c] = (c < n) ? src[r * n + c] : (((c - n) == r) ? 1.0F : 0.0F);
    }
  }
  threadgroup_barrier(mem_flags::mem_threadgroup);

  for (uint bs = 0; bs < n; bs += BLK) {
    const uint be  = min(bs + BLK, n);
    const uint blk = be - bs;

    // --- A) Reduce the diagonal block to the identity ------------------------------------------
    for (uint p = bs; p < be; ++p) {
      // Normalize the pivot row over the columns that are not reduced yet (the columns below bs are
      // zero in the pivot rows: the unit pattern left by the previous blocks puts their diagonal
      // inside the block, so every entry to the left is zero).
      if (tid.y == 0) {
        const float inv_pivot = 1.0F / gj[p][p];
        for (uint c = p + tid.x; c < 2 * n; c += tgs.x) {
          gj[p][c] *= inv_pivot;
        }
      }
      threadgroup_barrier(mem_flags::mem_threadgroup);

      // Eliminate the pivot column from the other rows OF THE BLOCK (the rows outside keep their
      // entries there: those are the multipliers step B collects).
      for (uint i = tid.y; i < blk; i += tgs.y) {
        const uint r = bs + i;
        if (r == p) {
          continue;
        }
        const float factor = gj[r][p];
        for (uint c = p + tid.x; c < 2 * n; c += tgs.x) {
          gj[r][c] -= factor * gj[p][c];
        }
      }
      threadgroup_barrier(mem_flags::mem_threadgroup);
    }

    // --- B) Save the multipliers of the rows outside the block ---------------------------------
    for (uint idx = tid_lin; idx < (n - blk) * BLK; idx += nthr) {
      const uint i = idx >> 3;  // index among the rows outside the pivot block
      const uint l = idx & 7u;
      const uint r = (i < bs) ? i : (i + blk);  // skip the pivot block rows
      mult[i][l] = (l < blk) ? gj[r][bs + l] : 0.0F;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    // --- C) Zero the pivot columns and update the rest with one rank-blk sweep ------------------
    for (uint idx = tid_lin; idx < (n - blk) * BLK; idx += nthr) {
      const uint i = idx >> 3;
      const uint l = idx & 7u;
      if (l < blk) {
        gj[(i < bs) ? i : (i + blk)][bs + l] = 0.0F;
      }
    }
    for (uint idx = tid_lin; idx < (n - blk) * (2 * n - be); idx += nthr) {
      const uint i = idx / (2 * n - be);
      const uint c = be + (idx % (2 * n - be));
      const uint r = (i < bs) ? i : (i + blk);
      float      acc = gj[r][c];
      for (uint l = 0; l < BLK; ++l) {
        acc -= mult[i][l] * gj[bs + l][c];
      }
      gj[r][c] = acc;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
  }

  // Write back the inverse.
  for (uint r = tid.y; r < n; r += tgs.y) {
    for (uint c = tid.x; c < n; c += tgs.x) {
      src[r * n + c] = gj[r][n + c];
    }
  }
}
