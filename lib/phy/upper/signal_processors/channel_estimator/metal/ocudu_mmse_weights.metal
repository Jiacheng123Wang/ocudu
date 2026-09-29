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

// ---------------------------------------------------------------------------------------------------
// B (dev doc 6.168): the same matrix product with A^-1 in THREADGROUP MEMORY.
//
// WHY. `mmse_weights` above reads A^-1 through `ap[k * p.L]` - a COLUMN walk with a row-sized stride -
// so every one of the (nout x L) threads misses the cache on most of its L loads, and the kernel's
// 13.18us/派发 (the largest single CE site, dev doc 6.65) is almost all memory stall: each output
// element costs just two FLOPs. The matrix is small and CONSTANT across the whole grid, which is
// exactly the shape threadgroup memory exists for.
//
// WHY ONE THREAD PER OUTPUT ELEMENT (and not a register-tiled GEMM). The result must stay BIT-EXACT
// with the kernel above and with the CPU reference (`gauss_jordan_invert` + `W = R_hp . A^-1`): the
// note on `mmse_weights` says the k-accumulation order is the contract. A register-tiled version
// would have to keep that order per element while sharing R_hp across a tile - possible, but it buys
// FLOPs this kernel does not need (the arithmetic is 2 FLOP per element). Hoisting A^-1 into
// threadgroup memory removes the stall WITHOUT touching the order: each thread still accumulates
// k = 0, 1, ... L-1 in ascending order, one product per step, from shared memory instead of device
// memory.
//
// LAYOUT. One threadgroup per TG_ROWS output rows of one system: thread tid accumulates the whole row
// `row_base + tid`, walking columns in ascending order. That gives LIP (loads in flight) independent
// shared-memory reads per thread instead of one dependent chain per column, and the A^-1 cooperative
// load makes the first L*L loads of the threadgroup coalesced (128 threads x float4) instead of
// strided by L.
//
// SIZING. L <= 72 (mmse_weights_params' own bound: npt <= 4, npf <= 18), so the buffer is at most
// 72*72*4 = 20736 bytes - inside the 32 KB threadgroup memory budget with room to spare. The loader
// is bounds-guarded, so an out-of-range L pads rather than writes past the buffer.

constant uint mmse_weights_tile_rows = 128; // rows one threadgroup covers (== its thread count)
constant uint mmse_weights_tile_max  = 72;  // == the L bound of mmse_weights_params

kernel void mmse_weights_tile(device const float*  r_hp [[buffer(0)]],  // [nof_systems][nout][L]
                              device const float*  a_inv [[buffer(1)]], // [nof_systems][L][L]
                              device float*        w     [[buffer(2)]], // [nof_systems][nout][L]
                              constant mmse_weights_params& p [[buffer(3)]],
                              uint tid  [[thread_position_in_threadgroup]],
                              uint tgid [[threadgroup_position_in_grid]])
{
  threadgroup float a_smem[mmse_weights_tile_max * mmse_weights_tile_max];

  // One threadgroup per (system, row block): the row blocks of a system are consecutive, so the system
  // is recovered with the same division the flat kernel used.
  const uint row_blocks = (p.nout + mmse_weights_tile_rows - 1u) / mmse_weights_tile_rows;
  const uint sys        = tgid / row_blocks;
  if (sys >= p.nof_systems) {
    return;
  }
  const uint row_base = (tgid % row_blocks) * mmse_weights_tile_rows;

  // Cooperative load of A^-1 for THIS system: consecutive threads take consecutive floats (coalesced),
  // bounds-guarded. Written before the barrier and read after it, so no thread can see a half-loaded
  // matrix.
  //
  // SCALAR, not float4: `threadgroup float a_smem[]` is only 4-byte aligned, and reinterpreting it as
  // float4 would be an unaligned access (the language would let it compile and the device would not
  // guarantee it). Alignment could be forced, but the load is L*L floats for a whole threadgroup - a
  // few hundred bytes per thread at most - so the vector width buys nothing worth an alignment
  // assumption.
  const uint nof_elem = p.L * p.L;
  for (uint i = tid; i < nof_elem; i += mmse_weights_tile_rows) {
    a_smem[i] = a_inv[sys * nof_elem + i];
  }
  threadgroup_barrier(mem_flags::mem_threadgroup);

  const uint row = row_base + tid;
  if (row >= p.nout) {
    return; // the barrier above is unconditional: every thread must reach it
  }

  device const float* rp = r_hp + sys * p.nout * p.L + row * p.L;
  // Ascending k, one product per step: the accumulation order of mmse_weights and of the CPU
  // reference. Only the SOURCE of the A^-1 element changed (shared instead of device memory), so the
  // arithmetic - and therefore the bits - is the same.
  // `acc += rp[k] * a_smem[...]`, NOT fma(): the kernel above and the CPU reference both round the
  // multiply and then the add, and the metallib is compiled with -fno-fast-math exactly so the
  // contraction does not happen behind our back (see ocudu_mmse_pilots.metal for the measurement that
  // made that flag a hard requirement). Writing fma() here would be a deliberate difference in the
  // last bit of every element.
  float acc = 0.0F;
  for (uint k = 0; k < p.L; ++k) {
    acc += rp[k] * a_smem[k * p.L + tid];
  }
  w[sys * p.nout * p.L + row * p.L + tid] = acc;
}
