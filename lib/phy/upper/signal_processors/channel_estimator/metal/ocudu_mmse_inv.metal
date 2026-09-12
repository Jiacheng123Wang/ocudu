// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// K1: batched matrix inversion for the 2D MMSE channel estimator.
//
// The matrix is A = R_pp + (sigma2 + ridge) I: R_pp is a correlation matrix (positive
// semi-definite) and sigma2 + ridge is strictly positive, so A is SYMMETRIC POSITIVE DEFINITE.
// Gaussian elimination therefore needs NO pivoting - the pivots stay above the smallest
// eigenvalue - which removes the serial pivot scan the previous version ran on a single thread.
//
// Parallel shape: one threadgroup per system, laid out as (column, row) so that the elimination
// of a pivot column spreads over the whole [A | I] block instead of one thread walking a full row
// of 2n threadgroup cells. Two barriers per pivot column against three, and the work per thread
// drops from O(n) cells to O(n / threads_y) cells.
//
// Matrix order n <= 36, nof_systems <= 8.

#include <metal_stdlib>
using namespace metal;

constant uint MAX_L  = 36;  // maximum matrix order
constant uint MAX_N2 = 72;  // 2 * MAX_L

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

  // [A | I] in threadgroup memory, then the inverse replaces A in place.
  threadgroup float gj[MAX_L][MAX_N2];

  device float* src = a + tgid.x * n * n;

  // Load [A | I]: every thread covers a stripe of the block.
  for (uint r = tid.y; r < n; r += tgs.y) {
    for (uint c = tid.x; c < 2 * n; c += tgs.x) {
      gj[r][c] = (c < n) ? src[r * n + c] : (((c - n) == r) ? 1.0F : 0.0F);
    }
  }
  threadgroup_barrier(mem_flags::mem_threadgroup);

  for (uint col = 0; col < n; ++col) {
    // Normalize the pivot row (no pivot search: see the header).
    const float inv_pivot = 1.0F / gj[col][col];
    if (tid.y == 0) {
      for (uint c = col + tid.x; c < 2 * n; c += tgs.x) {
        gj[col][c] *= inv_pivot;
      }
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    // Eliminate the pivot column from every other row; columns before `col` are already in
    // reduced form (unit vector) and are skipped.
    for (uint r = tid.y; r < n; r += tgs.y) {
      if (r == col) {
        continue;
      }
      const float factor = gj[r][col];
      if (factor == 0.0F) {
        continue;
      }
      for (uint c = col + tid.x; c < 2 * n; c += tgs.x) {
        gj[r][c] -= factor * gj[col][c];
      }
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
