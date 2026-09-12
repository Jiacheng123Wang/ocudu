// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// K3: device-side reformat of the K2 output into the equalizer's channel estimates.
//
// K2 writes one batch of time-frequency blocks: per (system, block) a row-major block of
// nout = nf * 14 complex outputs (real/imag interleaved floats), nf = b_prb * 12 subcarriers,
// symbol-major within the block. The equalizer does not consume that layout: for one OFDM symbol
// and one layer it wants the estimates of the allocated DATA resource elements only, packed in
// ascending subcarrier order and stored as cbf16 - see channel_equalizer::ch_est_list. This kernel
// performs that relayout and the float -> bfloat16 rounding on the GPU, so the host never walks
// the grid to build the equalizer's input (and, with the dispatch in the estimator's own command
// buffer, never has to read the grid back at all).
//
// The merged standard + edge-block batch (S-5c) shows up as two geometries in one batch: the
// standard blocks cover subcarriers [0, sc_tail_base) in systems [0, nof_layers) with blocks
// 0 .. n_blk-1, and the edge block covers the remaining subcarriers in systems
// [sys_tail, sys_tail + nof_layers) at block 0. Both keep the batch's row stride (nout_stride),
// which is the standard block's nout because that is what the padded edge system was computed
// with (its real positions are the first nf_tail * 14 rows).
//
// One thread per (layer, symbol, hop subcarrier): the destination index is the number of mask bits
// below that subcarrier, so every thread writes exactly one element and no atomics are needed.

#include <metal_stdlib>
using namespace metal;

struct mmse_reformat_params {
  uint nout_stride;  // Output positions per block in the batch (row length of h).
  uint n_blk;        // Blocks per system in the batch.
  uint nf_std;       // Subcarriers per standard block.
  uint sc_tail_base; // First subcarrier of the edge block (n_blk * nf_std; nof_sub when there is none).
  uint nf_tail;      // Subcarriers of the edge block (0 when the hop has no edge block).
  uint sys_tail;     // First system of the edge block (== nof_layers).
  uint nof_layers;
  uint nof_symbols;
  uint mask_words;   // 32-bit words per symbol mask.
  uint total_re;     // Compressed resource elements per layer (sum over symbols).
};

/// Round-to-nearest-even conversion to bfloat16, bit-identical to ocudu::to_bf16(): the 16 least
/// significant fraction bits are dropped, the remaining 7 are rounded half to even.
inline ushort ocudu_to_bf16(float value)
{
  const uint bits = as_type<uint>(value);
  return static_cast<ushort>((bits + 0x7fffu + ((bits >> 16) & 1u)) >> 16);
}

kernel void mmse_reformat(device const float* h [[buffer(0)]],   // [nof_systems][n_blk][2 * nout_stride]
                          device const uint*  masks [[buffer(1)]], // [nof_symbols][mask_words]
                          constant uint*      offsets [[buffer(2)]], // [nof_symbols] prefix RE counts
                          device ushort*      dst [[buffer(3)]],   // [nof_layers][total_re][2] bf16
                          constant mmse_reformat_params& p [[buffer(4)]],
                          uint gid [[thread_position_in_grid]])
{
  const uint nof_sub = p.sc_tail_base + p.nf_tail;
  const uint lay     = gid / (p.nof_symbols * nof_sub);
  const uint rem     = gid % (p.nof_symbols * nof_sub);
  const uint sym     = rem / nof_sub;
  const uint sc      = rem % nof_sub;
  if (lay >= p.nof_layers) {
    return;
  }

  // Resource elements outside the allocation, and the DM-RS positions of a DM-RS symbol, are not
  // part of the compressed layout the equalizer indexes by.
  device const uint* mask_row = masks + sym * p.mask_words;
  if ((mask_row[sc >> 5] & (1u << (sc & 31))) == 0) {
    return;
  }

  // Destination index: this subcarrier's rank among the set bits of the symbol's mask.
  const uint word = sc >> 5;
  uint       rank = 0;
  for (uint i = 0; i < word; ++i) {
    rank += popcount(mask_row[i]);
  }
  rank += popcount(mask_row[word] & ((1u << (sc & 31)) - 1u));

  uint nf, b, local_sc, sys;
  if (sc < p.sc_tail_base) {
    nf       = p.nf_std;
    b        = sc / nf;
    local_sc = sc % nf;
    sys      = lay;
  } else {
    nf       = p.nf_tail;
    b        = 0;
    local_sc = sc - p.sc_tail_base;
    sys      = p.sys_tail + lay;
  }

  device const float* hp =
      h + (sys * p.n_blk + b) * (2 * p.nout_stride) + 2 * (sym * nf + local_sc);

  const uint idx = lay * p.total_re + offsets[sym] + rank;
  dst[2 * idx]     = ocudu_to_bf16(hp[0]);
  dst[2 * idx + 1] = ocudu_to_bf16(hp[1]);
}
