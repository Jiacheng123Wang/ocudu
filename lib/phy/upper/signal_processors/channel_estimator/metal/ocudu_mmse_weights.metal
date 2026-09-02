// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// K1b: W = R_hp . A^-1 per system (real matrices). One thread per output element
// W[row][col], threadgroups stride over [system][nout x L] in 128-thread chunks.
// Consecutive threads map to consecutive columns of the same row: the A^-1 loads
// coalesce within a warp and the R_hp loads broadcast. nout <= 504, L <= 72,
// nof_systems <= 8.
//
// NOTE: the accumulation order over k is ascending, identical to the CPU reference
// (gauss_jordan_invert + W = R_hp . A^-1), so the results are bit-exact with the
// previous kernel and with the CPU path.

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
  // Flattened 1D grid: grid = nof_systems * ceil(nout * L / 128).
  const uint tgs_per_sys = (p.nout * p.L + 127u) / 128u;
  const uint sys         = tgid / tgs_per_sys;
  if (sys >= p.nof_systems) {
    return;
  }

  const uint gtid = (tgid % tgs_per_sys) * 128u + tid;
  const uint row  = gtid / p.L;
  const uint col  = gtid % p.L;
  if (row >= p.nout) {
    return;
  }

  device const float* rp = r_hp + sys * p.nout * p.L + row * p.L;
  device const float* ap = a_inv + sys * p.L * p.L + col;

  float acc = 0.0F;
  for (uint k = 0; k < p.L; ++k) {
    acc += rp[k] * ap[k * p.L];
  }

  w[sys * p.nout * p.L + row * p.L + col] = acc;
}
