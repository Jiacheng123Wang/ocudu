// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// K1b: W = R_hp . A^-1 per system (real matrices). One threadgroup per system,
// threads stride over the output rows. nout <= 504, L <= 36, nof_systems <= 8.

#include <metal_stdlib>
using namespace metal;

struct mmse_weights_params {
  uint nout;
  uint L;
  uint nof_systems;
};

kernel void mmse_weights(device const float*  r_hp [[buffer(0)]],  // [nof_systems][nout][L]
                         device const float*  a_inv [[buffer(1)]],  // [nof_systems][L][L]
                         device float*        w     [[buffer(2)]],  // [nof_systems][nout][L]
                         constant mmse_weights_params& p [[buffer(3)]],
                         uint tid  [[thread_position_in_threadgroup]],
                         uint tgid [[threadgroup_position_in_grid]])
{
  if (tgid >= p.nof_systems) {
    return;
  }

  device const float* rp = r_hp + tgid * p.nout * p.L;
  device const float* ap = a_inv + tgid * p.L * p.L;
  device float*       wp = w + tgid * p.nout * p.L;

  for (uint o = tid; o < p.nout; o += 128) {
    for (uint col = 0; col < p.L; ++col) {
      float acc = 0.0F;
      for (uint k = 0; k < p.L; ++k) {
        acc += rp[o * p.L + k] * ap[k * p.L + col];
      }
      wp[o * p.L + col] = acc;
    }
  }
}
