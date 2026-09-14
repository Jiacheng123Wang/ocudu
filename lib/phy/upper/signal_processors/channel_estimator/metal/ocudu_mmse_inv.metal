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

// The order limit is what fits the threadgroup memory: the augmented matrix is MAX_N x 2*MAX_N
// floats. 36 covered the 2-PRB blocks of the first deployments; the standard block of the air
// interface is 3 PRB with three DM-RS symbols, i.e. order 54, and 54 x 108 floats is 22.8 KiB -
// inside the 32 KiB budget, while 72 would not be. Raising it is what lets the REAL allocation be
// inverted on the device, which is a precondition for building the correlation matrices there too
// (the correlation must ride the same command buffer as the inversion, and an engine call only
// exists when the inversion is a device dispatch).
constant uint MAX_N  = 54;  // maximum matrix order
constant uint MAX_N2 = 108; // 2 * MAX_N
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

// ---------------------------------------------------------------------------------------------
// K1b: the same inverse, right-looking.
//
// K1 above updates the trailing submatrix once per BLOCK column (a rank-8 sweep) and reduces the
// diagonal block with the per-pivot steps, which costs two barriers per pivot. Measured on the
// production shape (one 54x54 system) that form is latency-bound: the elimination spends ~470us
// more than the ~10us of host Gauss-Jordan it is meant to replace, and the pivot phases are what
// the time goes into.
//
// This kernel keeps the same arithmetic (Gauss-Jordan on [A | I], no pivoting needed for the
// symmetric positive definite A = R_pp + (sigma2 + ridge) I) but runs a RIGHT-looking update: after
// a pivot row is normalized, every trailing element is updated with ONE rank-1 expression. Two
// barriers per pivot remain (the row must be scaled before it is used, and the update must finish
// before the next pivot is read), and that is the floor for a threadgroup-wide elimination - but the
// arithmetic per pivot drops to one multiply-add per trailing element instead of a rank-8 chain,
// which is what the latency was hiding behind.
//
// One threadgroup per system (like K1), so several systems run concurrently instead of sharing one
// threadgroup.
kernel void mmse_inv_rl(device float*       a           [[buffer(0)]],  // [nof_systems][n][n] row-major
                        constant uint&      n           [[buffer(1)]],
                        constant uint&      nof_systems [[buffer(2)]],
                        uint2               tid         [[thread_position_in_threadgroup]],
                        uint2               tgs         [[threads_per_threadgroup]],
                        uint2               tgid        [[threadgroup_position_in_grid]])
{
  if (tgid.x >= nof_systems) {
    return;
  }

  // [A | I], updated in place: the left half ends as the identity and the right half as the inverse.
  threadgroup float gj[MAX_N][MAX_N2];

  device float* src = a + tgid.x * n * n;

  for (uint r = tid.y; r < n; r += tgs.y) {
    for (uint c = tid.x; c < 2 * n; c += tgs.x) {
      gj[r][c] = (c < n) ? src[r * n + c] : (((c - n) == r) ? 1.0F : 0.0F);
    }
  }
  threadgroup_barrier(mem_flags::mem_threadgroup);

  const uint tid_lin = tid.y * tgs.x + tid.x;
  const uint nthr    = tgs.x * tgs.y;

  for (uint p = 0; p < n; ++p) {
    const uint n_cols = 2 * n - p;

    // 1) Scale the pivot row so the pivot becomes one (every column from the pivot on; the columns
    //    before it are already zero there, so scaling them would be a no-op).
    if (tid_lin < n_cols) {
      gj[p][p + tid_lin] *= 1.0F / gj[p][p];
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    // 2) Eliminate the pivot column from EVERY other row and update each of their trailing columns
    //    with the multiplier that row already carries - the classic in-place Gauss-Jordan step:
    //      row_r -= row_r[p] * row_p   (for every r != p, over the columns from the pivot on).
    //    Updating from the pivot column on is what keeps the eliminated column exactly zero: its
    //    own entry becomes 1 - 1 * 1 = 0.
    for (uint i = tid_lin; i < (n - 1) * n_cols; i += nthr) {
      const uint ri = i / n_cols;
      const uint r  = (ri < p) ? ri : (ri + 1);
      const uint c  = p + (i % n_cols);
      gj[r][c] -= gj[r][p] * gj[p][c];
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
  }

  for (uint r = tid.y; r < n; r += tgs.y) {
    for (uint c = tid.x; c < n; c += tgs.x) {
      src[r * n + c] = gj[r][n + c];
    }
  }
}

