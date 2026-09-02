// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// K2: batched block matrix multiplication for the 2D MMSE channel estimator:
// h = W . y per (system, time-frequency block). W is real-valued; y and h are
// complex (real/imag interleaved). One threadgroup per (system, block), one
// thread per block output position. nout <= 504, L <= 36, nof_systems <= 8.

#include <metal_stdlib>
using namespace metal;

struct mmse_apply_params {
  uint nout;
  uint L;
  uint nof_systems;
  uint nof_blocks;
};

kernel void mmse_apply(device const float*   w [[buffer(0)]],  // [nof_systems][nout][L]
                       device const float*   y [[buffer(1)]],  // [nof_systems][nof_blocks][2L]
                       device float*         h [[buffer(2)]],  // [nof_systems][nof_blocks][2nout]
                       constant mmse_apply_params& p [[buffer(3)]],
                       uint tid  [[thread_position_in_threadgroup]],
                       uint tgid [[threadgroup_position_in_grid]])
{
  // Flattened 1D grid: threadgroup = block + sys * nof_blocks.
  const uint block = tgid % p.nof_blocks;
  const uint sys   = tgid / p.nof_blocks;

  if (sys >= p.nof_systems || block >= p.nof_blocks || tid >= p.nout) {
    return;
  }

  device const float* wp = w + sys * p.nout * p.L + tid * p.L;
  device const float* yp = y + (sys * p.nof_blocks + block) * (2 * p.L);

  float acc_re = 0.0F;
  float acc_im = 0.0F;
  for (uint j = 0; j < p.L; ++j) {
    const float wj = wp[j];
    acc_re += wj * yp[2 * j];
    acc_im += wj * yp[2 * j + 1];
  }

  device float* hp = h + (sys * p.nof_blocks + block) * (2 * p.nout);
  hp[2 * tid]      = acc_re;
  hp[2 * tid + 1]  = acc_im;
}
