// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// K1: batched Gauss-Jordan matrix inversion for the 2D MMSE channel estimator.
// One threadgroup per system; [A | I] lives in threadgroup memory; the inverted
// matrix overwrites the input buffer. Matrix order n <= 36, nof_systems <= 8.

#include <metal_stdlib>
using namespace metal;

constant uint MAX_L = 36;   // maximum matrix order
constant uint MAX_N2 = 72;  // 2 * MAX_L

kernel void mmse_inv(device float*       a           [[buffer(0)]],  // [nof_systems][n][n] row-major
                     constant uint&      n           [[buffer(1)]],
                     constant uint&      nof_systems [[buffer(2)]],
                     uint                tid         [[thread_position_in_threadgroup]],
                     uint                tgid        [[threadgroup_position_in_grid]])
{
  if (tgid >= nof_systems || tid >= n) {
    return;
  }

  threadgroup float gj[MAX_L][MAX_N2];

  device float* src = a + tgid * n * n;

  // Load [A | I].
  for (uint c = 0; c < n; ++c) {
    gj[tid][c] = src[tid * n + c];
  }
  for (uint c = n; c < 2 * n; ++c) {
    gj[tid][c] = ((c - n) == tid) ? 1.0F : 0.0F;
  }
  threadgroup_barrier(mem_flags::mem_threadgroup);

  for (uint col = 0; col < n; ++col) {
    // Partial pivoting (serial scan by thread 0; n is tiny).
    if (tid == 0) {
      uint  pivot = col;
      float pv    = fabs(gj[col][col]);
      for (uint r = col + 1; r < n; ++r) {
        const float v = fabs(gj[r][col]);
        if (v > pv) {
          pv    = v;
          pivot = r;
        }
      }
      if (pivot != col) {
        for (uint c = 0; c < 2 * n; ++c) {
          const float tmp   = gj[col][c];
          gj[col][c]        = gj[pivot][c];
          gj[pivot][c]      = tmp;
        }
      }
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    const float pivot_val = gj[col][col];
    const float inv_pivot = 1.0F / pivot_val;

    // Normalize the pivot row.
    for (uint c = col + tid; c < 2 * n; c += n) {
      gj[col][c] *= inv_pivot;
    }
    // gj[col][col] itself becomes 1 (handled by the loop when tid == 0... make it explicit).
    if (tid == 0) {
      gj[col][col] = 1.0F;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    // Eliminate the pivot column from the other rows (each thread owns one row).
    if (tid != col) {
      const float factor = gj[tid][col];
      for (uint c = 0; c < 2 * n; ++c) {
        gj[tid][c] -= factor * gj[col][c];
      }
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
  }

  // Write back the inverse.
  for (uint c = 0; c < n; ++c) {
    src[tid * n + c] = gj[tid][n + c];
  }
}
